//===- Schedule.cpp - Calculate an optimized schedule ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass the isl to calculate a schedule that is optimized for parallelism
// and tileablility. The algorithm used in isl is an optimized version of the
// algorithm described in following paper:
//
// U. Bondhugula, A. Hartono, J. Ramanujam, and P. Sadayappan.
// A Practical Automatic Polyhedral Parallelizer and Locality Optimizer.
// In Proceedings of the 2008 ACM SIGPLAN Conference On Programming Language
// Design and Implementation, PLDI ’08, pages 101–113. ACM, 2008.
//===----------------------------------------------------------------------===//

#include "polly/ScheduleOptimizer.h"
#include "isl/aff.h"
#include "isl/band.h"
#include "isl/constraint.h"
#include "isl/map.h"
#include "isl/options.h"
#include "isl/schedule.h"
#include "isl/schedule_node.h"
#include "isl/space.h"
#include "polly/CodeGen/CodeGeneration.h"
#include "polly/DependenceInfo.h"
#include "polly/LinkAllPasses.h"
#include "polly/Options.h"
#include "polly/ScopInfo.h"
#include "polly/Support/GICHelper.h"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace polly;

#define DEBUG_TYPE "polly-opt-isl"

namespace polly {
bool DisablePollyTiling;
}
static cl::opt<bool, true>
    DisableTiling("polly-no-tiling",
                  cl::desc("Disable tiling in the scheduler"),
                  cl::location(polly::DisablePollyTiling), cl::init(false),
                  cl::ZeroOrMore, cl::cat(PollyCategory));

static cl::opt<std::string>
    OptimizeDeps("polly-opt-optimize-only",
                 cl::desc("Only a certain kind of dependences (all/raw)"),
                 cl::Hidden, cl::init("all"), cl::ZeroOrMore,
                 cl::cat(PollyCategory));

static cl::opt<std::string>
    SimplifyDeps("polly-opt-simplify-deps",
                 cl::desc("Dependences should be simplified (yes/no)"),
                 cl::Hidden, cl::init("yes"), cl::ZeroOrMore,
                 cl::cat(PollyCategory));

static cl::opt<int> MaxConstantTerm(
    "polly-opt-max-constant-term",
    cl::desc("The maximal constant term allowed (-1 is unlimited)"), cl::Hidden,
    cl::init(20), cl::ZeroOrMore, cl::cat(PollyCategory));

static cl::opt<int> MaxCoefficient(
    "polly-opt-max-coefficient",
    cl::desc("The maximal coefficient allowed (-1 is unlimited)"), cl::Hidden,
    cl::init(20), cl::ZeroOrMore, cl::cat(PollyCategory));

static cl::opt<std::string> FusionStrategy(
    "polly-opt-fusion", cl::desc("The fusion strategy to choose (min/max)"),
    cl::Hidden, cl::init("min"), cl::ZeroOrMore, cl::cat(PollyCategory));

static cl::opt<std::string>
    MaximizeBandDepth("polly-opt-maximize-bands",
                      cl::desc("Maximize the band depth (yes/no)"), cl::Hidden,
                      cl::init("yes"), cl::ZeroOrMore, cl::cat(PollyCategory));

static cl::opt<int> DefaultTileSize(
    "polly-default-tile-size",
    cl::desc("The default tile size (if not enough were provided by"
             " --polly-tile-sizes)"),
    cl::Hidden, cl::init(32), cl::ZeroOrMore, cl::cat(PollyCategory));

static cl::list<int> TileSizes("polly-tile-sizes",
                               cl::desc("A tile size"
                                        " for each loop dimension, filled with"
                                        " --polly-default-tile-size"),
                               cl::Hidden, cl::ZeroOrMore, cl::CommaSeparated,
                               cl::cat(PollyCategory));
namespace {

class IslScheduleOptimizer : public ScopPass {
public:
  static char ID;
  explicit IslScheduleOptimizer() : ScopPass(ID) { LastSchedule = nullptr; }

  ~IslScheduleOptimizer() { isl_schedule_free(LastSchedule); }

  bool runOnScop(Scop &S) override;
  void printScop(raw_ostream &OS, Scop &S) const override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  isl_schedule *LastSchedule;

  /// @brief Decide if the @p NewSchedule is profitable for @p S.
  ///
  /// @param S           The SCoP we optimize.
  /// @param NewSchedule The new schedule we computed.
  ///
  /// @return True, if we believe @p NewSchedule is an improvement for @p S.
  bool isProfitableSchedule(Scop &S, __isl_keep isl_union_map *NewSchedule);

  /// @brief Create a map that pre-vectorizes one scheduling dimension.
  ///
  /// getPrevectorMap creates a map that maps each input dimension to the same
  /// output dimension, except for the dimension DimToVectorize.
  /// DimToVectorize is strip mined by 'VectorWidth' and the newly created
  /// point loop of DimToVectorize is moved to the innermost level.
  ///
  /// Example (DimToVectorize=0, ScheduleDimensions=2, VectorWidth=4):
  ///
  /// | Before transformation
  /// |
  /// | A[i,j] -> [i,j]
  /// |
  /// | for (i = 0; i < 128; i++)
  /// |    for (j = 0; j < 128; j++)
  /// |      A(i,j);
  ///
  ///   Prevector map:
  ///   [i,j] -> [it,j,ip] : it % 4 = 0 and it <= ip <= it + 3 and i = ip
  ///
  /// | After transformation:
  /// |
  /// | A[i,j] -> [it,j,ip] : it % 4 = 0 and it <= ip <= it + 3 and i = ip
  /// |
  /// | for (it = 0; it < 128; it+=4)
  /// |    for (j = 0; j < 128; j++)
  /// |      for (ip = max(0,it); ip < min(128, it + 3); ip++)
  /// |        A(ip,j);
  ///
  /// The goal of this transformation is to create a trivially vectorizable
  /// loop.  This means a parallel loop at the innermost level that has a
  /// constant number of iterations corresponding to the target vector width.
  ///
  /// This transformation creates a loop at the innermost level. The loop has
  /// a constant number of iterations, if the number of loop iterations at
  /// DimToVectorize can be divided by VectorWidth. The default VectorWidth is
  /// currently constant and not yet target specific. This function does not
  /// reason about parallelism.
  static __isl_give isl_map *getPrevectorMap(isl_ctx *ctx, int DimToVectorize,
                                             int ScheduleDimensions,
                                             int VectorWidth = 4);

  /// @brief Apply additional optimizations on the bands in the schedule tree.
  ///
  /// We are looking for an innermost band node and apply the following
  /// transformations:
  ///
  ///  - Tile the band
  ///      - if the band is tileable
  ///      - if the band has more than one loop dimension
  ///
  ///  - Prevectorize the point loop of the tile
  ///      - if vectorization is enabled
  ///
  /// @param Node The schedule node to (possibly) optimize.
  /// @param User A pointer to forward some use information (currently unused).
  static isl_schedule_node *optimizeBand(isl_schedule_node *Node, void *User);

  static __isl_give isl_union_map *
  getScheduleMap(__isl_keep isl_schedule *Schedule);

  using llvm::Pass::doFinalization;

  virtual bool doFinalization() override {
    isl_schedule_free(LastSchedule);
    LastSchedule = nullptr;
    return true;
  }
};
}

char IslScheduleOptimizer::ID = 0;

__isl_give isl_map *
IslScheduleOptimizer::getPrevectorMap(isl_ctx *ctx, int DimToVectorize,
                                      int ScheduleDimensions, int VectorWidth) {
  isl_space *Space;
  isl_local_space *LocalSpace, *LocalSpaceRange;
  isl_set *Modulo;
  isl_map *TilingMap;
  isl_constraint *c;
  isl_aff *Aff;
  int PointDimension; /* ip */
  int TileDimension;  /* it */
  isl_val *VectorWidthMP;

  assert(0 <= DimToVectorize && DimToVectorize < ScheduleDimensions);

  Space = isl_space_alloc(ctx, 0, ScheduleDimensions, ScheduleDimensions + 1);
  TilingMap = isl_map_universe(isl_space_copy(Space));
  LocalSpace = isl_local_space_from_space(Space);
  PointDimension = ScheduleDimensions;
  TileDimension = DimToVectorize;

  // Create an identity map for everything except DimToVectorize and map
  // DimToVectorize to the point loop at the innermost dimension.
  for (int i = 0; i < ScheduleDimensions; i++) {
    c = isl_equality_alloc(isl_local_space_copy(LocalSpace));
    c = isl_constraint_set_coefficient_si(c, isl_dim_in, i, -1);

    if (i == DimToVectorize)
      c = isl_constraint_set_coefficient_si(c, isl_dim_out, PointDimension, 1);
    else
      c = isl_constraint_set_coefficient_si(c, isl_dim_out, i, 1);

    TilingMap = isl_map_add_constraint(TilingMap, c);
  }

  // it % 'VectorWidth' = 0
  LocalSpaceRange = isl_local_space_range(isl_local_space_copy(LocalSpace));
  Aff = isl_aff_zero_on_domain(LocalSpaceRange);
  Aff = isl_aff_set_constant_si(Aff, VectorWidth);
  Aff = isl_aff_set_coefficient_si(Aff, isl_dim_in, TileDimension, 1);
  VectorWidthMP = isl_val_int_from_si(ctx, VectorWidth);
  Aff = isl_aff_mod_val(Aff, VectorWidthMP);
  Modulo = isl_pw_aff_zero_set(isl_pw_aff_from_aff(Aff));
  TilingMap = isl_map_intersect_range(TilingMap, Modulo);

  // it <= ip
  c = isl_inequality_alloc(isl_local_space_copy(LocalSpace));
  isl_constraint_set_coefficient_si(c, isl_dim_out, TileDimension, -1);
  isl_constraint_set_coefficient_si(c, isl_dim_out, PointDimension, 1);
  TilingMap = isl_map_add_constraint(TilingMap, c);

  // ip <= it + ('VectorWidth' - 1)
  c = isl_inequality_alloc(LocalSpace);
  isl_constraint_set_coefficient_si(c, isl_dim_out, TileDimension, 1);
  isl_constraint_set_coefficient_si(c, isl_dim_out, PointDimension, -1);
  isl_constraint_set_constant_si(c, VectorWidth - 1);
  TilingMap = isl_map_add_constraint(TilingMap, c);

  return TilingMap;
}

isl_schedule_node *IslScheduleOptimizer::optimizeBand(isl_schedule_node *Node,
                                                      void *User) {
  if (isl_schedule_node_get_type(Node) != isl_schedule_node_band)
    return Node;

  if (isl_schedule_node_n_children(Node) != 1)
    return Node;

  if (!isl_schedule_node_band_get_permutable(Node))
    return Node;

  auto Space = isl_schedule_node_band_get_space(Node);
  auto Dims = isl_space_dim(Space, isl_dim_set);

  if (Dims <= 1) {
    isl_space_free(Space);
    return Node;
  }

  auto Child = isl_schedule_node_get_child(Node, 0);
  auto Type = isl_schedule_node_get_type(Child);
  isl_schedule_node_free(Child);

  if (Type != isl_schedule_node_leaf) {
    isl_space_free(Space);
    return Node;
  }

  auto Sizes = isl_multi_val_zero(Space);
  auto Ctx = isl_schedule_node_get_ctx(Node);

  for (unsigned i = 0; i < Dims; i++) {
    auto tileSize = TileSizes.size() > i ? TileSizes[i] : DefaultTileSize;
    Sizes = isl_multi_val_set_val(Sizes, i, isl_val_int_from_si(Ctx, tileSize));
  }

  isl_schedule_node *Res;

  if (DisableTiling) {
    isl_multi_val_free(Sizes);
    Res = Node;
  } else {
    Res = isl_schedule_node_band_tile(Node, Sizes);
  }

  if (PollyVectorizerChoice == VECTORIZER_NONE)
    return Res;

  Child = isl_schedule_node_get_child(Res, 0);
  auto ChildSchedule = isl_schedule_node_band_get_partial_schedule(Child);

  for (int i = Dims - 1; i >= 0; i--) {
    if (isl_schedule_node_band_member_get_coincident(Child, i)) {
      auto TileMap = IslScheduleOptimizer::getPrevectorMap(Ctx, i, Dims);
      auto TileUMap = isl_union_map_from_map(TileMap);
      auto ChildSchedule2 = isl_union_map_apply_range(
          isl_union_map_from_multi_union_pw_aff(ChildSchedule), TileUMap);
      ChildSchedule = isl_multi_union_pw_aff_from_union_map(ChildSchedule2);
      break;
    }
  }

  isl_schedule_node_free(Res);
  Res = isl_schedule_node_delete(Child);
  Res = isl_schedule_node_insert_partial_schedule(Res, ChildSchedule);
  return Res;
}

__isl_give isl_union_map *
IslScheduleOptimizer::getScheduleMap(__isl_keep isl_schedule *Schedule) {
  isl_schedule_node *Root = isl_schedule_get_root(Schedule);
  Root = isl_schedule_node_map_descendant(
      Root, IslScheduleOptimizer::optimizeBand, NULL);
  auto ScheduleMap = isl_schedule_node_get_subtree_schedule_union_map(Root);
  ScheduleMap = isl_union_map_detect_equalities(ScheduleMap);
  isl_schedule_node_free(Root);
  return ScheduleMap;
}

bool IslScheduleOptimizer::isProfitableSchedule(
    Scop &S, __isl_keep isl_union_map *NewSchedule) {
  // To understand if the schedule has been optimized we check if the schedule
  // has changed at all.
  // TODO: We can improve this by tracking if any necessarily beneficial
  // transformations have been performed. This can e.g. be tiling, loop
  // interchange, or ...) We can track this either at the place where the
  // transformation has been performed or, in case of automatic ILP based
  // optimizations, by comparing (yet to be defined) performance metrics
  // before/after the scheduling optimizer
  // (e.g., #stride-one accesses)
  isl_union_map *OldSchedule = S.getSchedule();
  bool changed = !isl_union_map_is_equal(OldSchedule, NewSchedule);
  isl_union_map_free(OldSchedule);
  return changed;
}

bool IslScheduleOptimizer::runOnScop(Scop &S) {

  // Skip empty SCoPs but still allow code generation as it will delete the
  // loops present but not needed.
  if (S.getSize() == 0) {
    S.markAsOptimized();
    return false;
  }

  const Dependences &D = getAnalysis<DependenceInfo>().getDependences();

  if (!D.hasValidDependences())
    return false;

  isl_schedule_free(LastSchedule);
  LastSchedule = nullptr;

  // Build input data.
  int ValidityKinds =
      Dependences::TYPE_RAW | Dependences::TYPE_WAR | Dependences::TYPE_WAW;
  int ProximityKinds;

  if (OptimizeDeps == "all")
    ProximityKinds =
        Dependences::TYPE_RAW | Dependences::TYPE_WAR | Dependences::TYPE_WAW;
  else if (OptimizeDeps == "raw")
    ProximityKinds = Dependences::TYPE_RAW;
  else {
    errs() << "Do not know how to optimize for '" << OptimizeDeps << "'"
           << " Falling back to optimizing all dependences.\n";
    ProximityKinds =
        Dependences::TYPE_RAW | Dependences::TYPE_WAR | Dependences::TYPE_WAW;
  }

  isl_union_set *Domain = S.getDomains();

  if (!Domain)
    return false;

  isl_union_map *Validity = D.getDependences(ValidityKinds);
  isl_union_map *Proximity = D.getDependences(ProximityKinds);

  // Simplify the dependences by removing the constraints introduced by the
  // domains. This can speed up the scheduling time significantly, as large
  // constant coefficients will be removed from the dependences. The
  // introduction of some additional dependences reduces the possible
  // transformations, but in most cases, such transformation do not seem to be
  // interesting anyway. In some cases this option may stop the scheduler to
  // find any schedule.
  if (SimplifyDeps == "yes") {
    Validity = isl_union_map_gist_domain(Validity, isl_union_set_copy(Domain));
    Validity = isl_union_map_gist_range(Validity, isl_union_set_copy(Domain));
    Proximity =
        isl_union_map_gist_domain(Proximity, isl_union_set_copy(Domain));
    Proximity = isl_union_map_gist_range(Proximity, isl_union_set_copy(Domain));
  } else if (SimplifyDeps != "no") {
    errs() << "warning: Option -polly-opt-simplify-deps should either be 'yes' "
              "or 'no'. Falling back to default: 'yes'\n";
  }

  DEBUG(dbgs() << "\n\nCompute schedule from: ");
  DEBUG(dbgs() << "Domain := " << stringFromIslObj(Domain) << ";\n");
  DEBUG(dbgs() << "Proximity := " << stringFromIslObj(Proximity) << ";\n");
  DEBUG(dbgs() << "Validity := " << stringFromIslObj(Validity) << ";\n");

  int IslFusionStrategy;

  if (FusionStrategy == "max") {
    IslFusionStrategy = ISL_SCHEDULE_FUSE_MAX;
  } else if (FusionStrategy == "min") {
    IslFusionStrategy = ISL_SCHEDULE_FUSE_MIN;
  } else {
    errs() << "warning: Unknown fusion strategy. Falling back to maximal "
              "fusion.\n";
    IslFusionStrategy = ISL_SCHEDULE_FUSE_MAX;
  }

  int IslMaximizeBands;

  if (MaximizeBandDepth == "yes") {
    IslMaximizeBands = 1;
  } else if (MaximizeBandDepth == "no") {
    IslMaximizeBands = 0;
  } else {
    errs() << "warning: Option -polly-opt-maximize-bands should either be 'yes'"
              " or 'no'. Falling back to default: 'yes'\n";
    IslMaximizeBands = 1;
  }

  isl_options_set_schedule_fuse(S.getIslCtx(), IslFusionStrategy);
  isl_options_set_schedule_maximize_band_depth(S.getIslCtx(), IslMaximizeBands);
  isl_options_set_schedule_max_constant_term(S.getIslCtx(), MaxConstantTerm);
  isl_options_set_schedule_max_coefficient(S.getIslCtx(), MaxCoefficient);
  isl_options_set_tile_scale_tile_loops(S.getIslCtx(), 0);

  isl_options_set_on_error(S.getIslCtx(), ISL_ON_ERROR_CONTINUE);

  isl_schedule_constraints *ScheduleConstraints;
  ScheduleConstraints = isl_schedule_constraints_on_domain(Domain);
  ScheduleConstraints =
      isl_schedule_constraints_set_proximity(ScheduleConstraints, Proximity);
  ScheduleConstraints = isl_schedule_constraints_set_validity(
      ScheduleConstraints, isl_union_map_copy(Validity));
  ScheduleConstraints =
      isl_schedule_constraints_set_coincidence(ScheduleConstraints, Validity);
  isl_schedule *Schedule;
  Schedule = isl_schedule_constraints_compute_schedule(ScheduleConstraints);
  isl_options_set_on_error(S.getIslCtx(), ISL_ON_ERROR_ABORT);

  // In cases the scheduler is not able to optimize the code, we just do not
  // touch the schedule.
  if (!Schedule)
    return false;

  DEBUG(dbgs() << "Schedule := " << stringFromIslObj(Schedule) << ";\n");

  isl_union_map *NewSchedule = getScheduleMap(Schedule);

  // Check if the optimizations performed were profitable, otherwise exit early.
  if (!isProfitableSchedule(S, NewSchedule)) {
    isl_schedule_free(Schedule);
    isl_union_map_free(NewSchedule);
    return false;
  }

  S.markAsOptimized();

  for (ScopStmt *Stmt : S) {
    isl_map *StmtSchedule;
    isl_set *Domain = Stmt->getDomain();
    isl_union_map *StmtBand;
    StmtBand = isl_union_map_intersect_domain(isl_union_map_copy(NewSchedule),
                                              isl_union_set_from_set(Domain));
    if (isl_union_map_is_empty(StmtBand)) {
      StmtSchedule = isl_map_from_domain(isl_set_empty(Stmt->getDomainSpace()));
      isl_union_map_free(StmtBand);
    } else {
      assert(isl_union_map_n_map(StmtBand) == 1);
      StmtSchedule = isl_map_from_union_map(StmtBand);
    }

    Stmt->setScattering(StmtSchedule);
  }

  isl_schedule_free(Schedule);
  isl_union_map_free(NewSchedule);
  return false;
}

void IslScheduleOptimizer::printScop(raw_ostream &OS, Scop &) const {
  isl_printer *p;
  char *ScheduleStr;

  OS << "Calculated schedule:\n";

  if (!LastSchedule) {
    OS << "n/a\n";
    return;
  }

  p = isl_printer_to_str(isl_schedule_get_ctx(LastSchedule));
  p = isl_printer_print_schedule(p, LastSchedule);
  ScheduleStr = isl_printer_get_str(p);
  isl_printer_free(p);

  OS << ScheduleStr << "\n";
}

void IslScheduleOptimizer::getAnalysisUsage(AnalysisUsage &AU) const {
  ScopPass::getAnalysisUsage(AU);
  AU.addRequired<DependenceInfo>();
}

Pass *polly::createIslScheduleOptimizerPass() {
  return new IslScheduleOptimizer();
}

INITIALIZE_PASS_BEGIN(IslScheduleOptimizer, "polly-opt-isl",
                      "Polly - Optimize schedule of SCoP", false, false);
INITIALIZE_PASS_DEPENDENCY(DependenceInfo);
INITIALIZE_PASS_DEPENDENCY(ScopInfo);
INITIALIZE_PASS_END(IslScheduleOptimizer, "polly-opt-isl",
                    "Polly - Optimize schedule of SCoP", false, false)
