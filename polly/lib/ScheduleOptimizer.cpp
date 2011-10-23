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

#include "polly/Cloog.h"
#include "polly/LinkAllPasses.h"
#include "polly/CodeGeneration.h"
#include "polly/Support/GICHelper.h"
#include "polly/Dependences.h"
#include "polly/ScopInfo.h"

#include "isl/space.h"
#include "isl/map.h"
#include "isl/constraint.h"
#include "isl/schedule.h"
#include "isl/band.h"

#define DEBUG_TYPE "polly-optimize-isl"
#include "llvm/Support/Debug.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;
using namespace polly;

namespace polly {
  bool DisablePollyTiling;
}
static cl::opt<bool, true>
DisableTiling("polly-no-tiling",
	      cl::desc("Disable tiling in the scheduler"), cl::Hidden,
              cl::location(polly::DisablePollyTiling), cl::init(false));

namespace {

  class IslScheduleOptimizer : public ScopPass {

  public:
    static char ID;
    explicit IslScheduleOptimizer() : ScopPass(ID) {}

    virtual bool runOnScop(Scop &S);
    void printScop(llvm::raw_ostream &OS) const;
    void getAnalysisUsage(AnalysisUsage &AU) const;
  };

}

char IslScheduleOptimizer::ID = 0;

static int getSingleMap(__isl_take isl_map *map, void *user) {
  isl_map **singleMap = (isl_map **) user;
  *singleMap = map;

  return 0;
}

static void extendScattering(Scop &S, unsigned NewDimensions) {
  for (Scop::iterator SI = S.begin(), SE = S.end(); SI != SE; ++SI) {
    ScopStmt *Stmt = *SI;

    if (Stmt->isFinalRead())
      continue;

    unsigned OldDimensions = Stmt->getNumScattering();
    isl_space *Space;
    isl_basic_map *ChangeScattering;

    Space = isl_space_alloc(Stmt->getIslCtx(), 0, OldDimensions, NewDimensions);
    ChangeScattering = isl_basic_map_universe(isl_space_copy(Space));
    isl_local_space *LocalSpace = isl_local_space_from_space(Space);

    for (unsigned i = 0; i < OldDimensions; i++) {
      isl_constraint *c = isl_equality_alloc(isl_local_space_copy(LocalSpace));
      isl_constraint_set_coefficient_si(c, isl_dim_in, i, 1);
      isl_constraint_set_coefficient_si(c, isl_dim_out, i, -1);
      ChangeScattering = isl_basic_map_add_constraint(ChangeScattering, c);
    }

    for (unsigned i = OldDimensions; i < NewDimensions; i++) {
      isl_constraint *c = isl_equality_alloc(isl_local_space_copy(LocalSpace));
      isl_constraint_set_coefficient_si(c, isl_dim_out, i, 1);
      ChangeScattering = isl_basic_map_add_constraint(ChangeScattering, c);
    }

    isl_map *ChangeScatteringMap = isl_map_from_basic_map(ChangeScattering);

    ChangeScatteringMap = isl_map_align_params(ChangeScatteringMap,
                                               S.getParamSpace());
    isl_map *NewScattering = isl_map_apply_range(Stmt->getScattering(),
                                                 ChangeScatteringMap);
    Stmt->setScattering(NewScattering);
    isl_local_space_free(LocalSpace);
  }
}

// getTileMap - Create a map that describes a n-dimensonal tiling.
//
// getTileMap creates a map from a n-dimensional scattering space into an
// 2*n-dimensional scattering space. The map describes a rectangular tiling.
//
// Example:
//   scheduleDimensions = 2, parameterDimensions = 1, tileSize = 32
//
//   tileMap := [p0] -> {[s0, s1] -> [t0, t1, s0, s1]:
//                        t0 % 32 = 0 and t0 <= s0 < t0 + 32 and
//                        t1 % 32 = 0 and t1 <= s1 < t1 + 32}
//
//  Before tiling:
//
//  for (i = 0; i < N; i++)
//    for (j = 0; j < M; j++)
//	S(i,j)
//
//  After tiling:
//
//  for (t_i = 0; t_i < N; i+=32)
//    for (t_j = 0; t_j < M; j+=32)
//	for (i = t_i; i < min(t_i + 32, N); i++)  | Unknown that N % 32 = 0
//	  for (j = t_j; j < t_j + 32; j++)        |   Known that M % 32 = 0
//	    S(i,j)
//
static isl_basic_map *getTileMap(isl_ctx *ctx, int scheduleDimensions,
				 isl_space *SpaceModel, int tileSize = 32) {
  // We construct
  //
  // tileMap := [p0] -> {[s0, s1] -> [t0, t1, p0, p1, a0, a1]:
  //	                  s0 = a0 * 32 and s0 = p0 and t0 <= p0 < t0 + 32 and
  //	                  s1 = a1 * 32 and s1 = p1 and t1 <= p1 < t1 + 32}
  //
  // and project out the auxilary dimensions a0 and a1.
  isl_space *Space = isl_space_alloc(ctx, 0, scheduleDimensions,
                                     scheduleDimensions * 3);
  isl_basic_map *tileMap = isl_basic_map_universe(isl_space_copy(Space));

  isl_local_space *LocalSpace = isl_local_space_from_space(Space);

  for (int x = 0; x < scheduleDimensions; x++) {
    int sX = x;
    int tX = x;
    int pX = scheduleDimensions + x;
    int aX = 2 * scheduleDimensions + x;

    isl_constraint *c;

    // sX = aX * tileSize;
    c = isl_equality_alloc(isl_local_space_copy(LocalSpace));
    isl_constraint_set_coefficient_si(c, isl_dim_out, sX, 1);
    isl_constraint_set_coefficient_si(c, isl_dim_out, aX, -tileSize);
    tileMap = isl_basic_map_add_constraint(tileMap, c);

    // pX = sX;
    c = isl_equality_alloc(isl_local_space_copy(LocalSpace));
    isl_constraint_set_coefficient_si(c, isl_dim_out, pX, 1);
    isl_constraint_set_coefficient_si(c, isl_dim_in, sX, -1);
    tileMap = isl_basic_map_add_constraint(tileMap, c);

    // tX <= pX
    c = isl_inequality_alloc(isl_local_space_copy(LocalSpace));
    isl_constraint_set_coefficient_si(c, isl_dim_out, pX, 1);
    isl_constraint_set_coefficient_si(c, isl_dim_out, tX, -1);
    tileMap = isl_basic_map_add_constraint(tileMap, c);

    // pX <= tX + (tileSize - 1)
    c = isl_inequality_alloc(isl_local_space_copy(LocalSpace));
    isl_constraint_set_coefficient_si(c, isl_dim_out, tX, 1);
    isl_constraint_set_coefficient_si(c, isl_dim_out, pX, -1);
    isl_constraint_set_constant_si(c, tileSize - 1);
    tileMap = isl_basic_map_add_constraint(tileMap, c);
  }

  // Project out auxilary dimensions.
  //
  // The auxilary dimensions are transformed into existentially quantified ones.
  // This reduces the number of visible scattering dimensions and allows Cloog
  // to produces better code.
  tileMap = isl_basic_map_project_out(tileMap, isl_dim_out,
				      2 * scheduleDimensions,
				      scheduleDimensions);
  isl_local_space_free(LocalSpace);
  return tileMap;
}

isl_union_map *getTiledPartialSchedule(isl_band *band) {
  isl_union_map *partialSchedule;
  int scheduleDimensions;
  isl_ctx *ctx;
  isl_space *Space;
  isl_basic_map *tileMap;
  isl_union_map *tileUnionMap;

  partialSchedule = isl_band_get_partial_schedule(band);

  if (!DisableTiling) {
    ctx = isl_union_map_get_ctx(partialSchedule);
    Space= isl_union_map_get_space(partialSchedule);
    scheduleDimensions = isl_band_n_member(band);

    tileMap = getTileMap(ctx, scheduleDimensions, Space);
    tileUnionMap = isl_union_map_from_map(isl_map_from_basic_map(tileMap));
    tileUnionMap = isl_union_map_align_params(tileUnionMap, Space);
    partialSchedule = isl_union_map_apply_range(partialSchedule, tileUnionMap);
  }

  return partialSchedule;
}

static isl_map *getPrevectorMap(isl_ctx *ctx, int vectorDimension,
				int scheduleDimensions,
				int parameterDimensions,
				int vectorWidth = 4) {
  assert (0 <= vectorDimension && vectorDimension < scheduleDimensions);

  isl_space *Space = isl_space_alloc(ctx, parameterDimensions,
                                     scheduleDimensions, scheduleDimensions + 2);
  isl_basic_map *tilingMap = isl_basic_map_universe(isl_space_copy(Space));

  isl_constraint *c;

  isl_local_space *LocalSpace = isl_local_space_from_space(Space);

  for (int i = 0; i < vectorDimension; i++) {
    c = isl_equality_alloc(isl_local_space_copy(LocalSpace));
    isl_constraint_set_coefficient_si(c, isl_dim_in, i, -1);
    isl_constraint_set_coefficient_si(c, isl_dim_out, i, 1);
    tilingMap = isl_basic_map_add_constraint(tilingMap, c);
  }

  for (int i = vectorDimension + 1; i < scheduleDimensions; i++) {
    c = isl_equality_alloc(isl_local_space_copy(LocalSpace));
    isl_constraint_set_coefficient_si(c, isl_dim_in, i, -1);
    isl_constraint_set_coefficient_si(c, isl_dim_out, i, 1);
    tilingMap = isl_basic_map_add_constraint(tilingMap, c);
  }

  int stepDimension = scheduleDimensions;
  int auxilaryDimension = scheduleDimensions + 1;

  c = isl_equality_alloc(isl_local_space_copy(LocalSpace));
  isl_constraint_set_coefficient_si(c, isl_dim_out, vectorDimension, 1);
  isl_constraint_set_coefficient_si(c, isl_dim_out, auxilaryDimension,
				    -vectorWidth);
  tilingMap = isl_basic_map_add_constraint(tilingMap, c);

  c = isl_equality_alloc(isl_local_space_copy(LocalSpace));
  isl_constraint_set_coefficient_si(c, isl_dim_in, vectorDimension, -1);
  isl_constraint_set_coefficient_si(c, isl_dim_out, stepDimension, 1);
  tilingMap = isl_basic_map_add_constraint(tilingMap, c);

  c = isl_inequality_alloc(isl_local_space_copy(LocalSpace));
  isl_constraint_set_coefficient_si(c, isl_dim_out, vectorDimension, -1);
  isl_constraint_set_coefficient_si(c, isl_dim_out, stepDimension, 1);
  tilingMap = isl_basic_map_add_constraint(tilingMap, c);

  c = isl_inequality_alloc(LocalSpace);
  isl_constraint_set_coefficient_si(c, isl_dim_out, vectorDimension, 1);
  isl_constraint_set_coefficient_si(c, isl_dim_out, stepDimension, -1);
  isl_constraint_set_constant_si(c, vectorWidth- 1);
  tilingMap = isl_basic_map_add_constraint(tilingMap, c);

  // Project out auxilary dimensions (introduced to ensure 'ii % tileSize = 0')
  //
  // The real dimensions are transformed into existentially quantified ones.
  // This reduces the number of visible scattering dimensions.  Also, Cloog
  // produces better code, if auxilary dimensions are existentially quantified.
  tilingMap = isl_basic_map_project_out(tilingMap, isl_dim_out,
					scheduleDimensions + 1, 1);

  return isl_map_from_basic_map(tilingMap);
}

// tileBandList - Tile all bands contained in a band forest.
//
// Recursively walk the band forest and tile all bands in the forest. Return
// a schedule that describes the tiled scattering.
static isl_union_map *tileBandList(isl_band_list *blist) {
  int numBands = isl_band_list_n_band(blist);

  isl_union_map *finalSchedule = 0;

  for (int i = 0; i < numBands; i++) {
    isl_band *band;
    isl_union_map *partialSchedule;
    band = isl_band_list_get_band(blist, i);
    partialSchedule = getTiledPartialSchedule(band);
    int scheduleDimensions = isl_band_n_member(band);
    isl_space *Space = isl_union_map_get_space(partialSchedule);


    if (isl_band_has_children(band)) {
      isl_band_list *children = isl_band_get_children(band);
      isl_union_map *suffixSchedule = tileBandList(children);
      partialSchedule = isl_union_map_flat_range_product(partialSchedule,
							 suffixSchedule);
      isl_band_list_free(children);
    } else if (EnablePollyVector) {
      isl_map *tileMap;
      isl_union_map *tileUnionMap;
      isl_ctx *ctx;

      ctx = isl_union_map_get_ctx(partialSchedule);
      for (int i = scheduleDimensions - 1 ;  i >= 0 ; i--) {
	if (isl_band_member_is_zero_distance(band, i)) {
	  tileMap = getPrevectorMap(ctx, scheduleDimensions + i,
				    scheduleDimensions * 2, 0);
	  tileUnionMap = isl_union_map_from_map(tileMap);
          tileUnionMap = isl_union_map_align_params(tileUnionMap,
                                                    isl_space_copy(Space));
	  partialSchedule = isl_union_map_apply_range(partialSchedule,
						      tileUnionMap);
	  break;
	}
      }
    }

    if (finalSchedule)
      finalSchedule = isl_union_map_union(finalSchedule, partialSchedule);
    else
      finalSchedule = partialSchedule;

    isl_band_free(band);
    isl_space_free(Space);
  }

  return finalSchedule;
}

static isl_union_map *tileSchedule(isl_schedule *schedule) {
  isl_band_list *blist = isl_schedule_get_band_forest(schedule);
  isl_union_map *tiledSchedule = tileBandList(blist);
  isl_band_list_free(blist);
  return tiledSchedule;
}

bool IslScheduleOptimizer::runOnScop(Scop &S) {
  Dependences *D = &getAnalysis<Dependences>();

  // Build input data.
  int dependencyKinds = Dependences::TYPE_RAW
                          | Dependences::TYPE_WAR
                          | Dependences::TYPE_WAW;

  isl_union_map *validity = D->getDependences(dependencyKinds);
  isl_union_map *proximity = D->getDependences(dependencyKinds);
  isl_union_set *domain = NULL;

  for (Scop::iterator SI = S.begin(), SE = S.end(); SI != SE; ++SI)
    if ((*SI)->isFinalRead())
      continue;
    else if (!domain)
      domain = isl_union_set_from_set((*SI)->getDomain());
    else
      domain = isl_union_set_union(domain,
        isl_union_set_from_set((*SI)->getDomain()));

  if (!domain)
    return false;

  DEBUG(dbgs() << "\n\nCompute schedule from: ");
  DEBUG(dbgs() << "Domain := "; isl_union_set_dump(domain); dbgs() << ";\n");
  DEBUG(dbgs() << "Proximity := "; isl_union_map_dump(proximity);
        dbgs() << ";\n");
  DEBUG(dbgs() << "Validity := "; isl_union_map_dump(validity);
        dbgs() << ";\n");

  isl_schedule *schedule;

  schedule  = isl_union_set_compute_schedule(domain, validity, proximity);

  DEBUG(dbgs() << "Computed schedule: ");
  DEBUG(dbgs() << stringFromIslObj(schedule));
  DEBUG(dbgs() << "Individual bands: ");

  isl_union_map *tiledSchedule = tileSchedule(schedule);

  for (Scop::iterator SI = S.begin(), SE = S.end(); SI != SE; ++SI) {
    ScopStmt *stmt = *SI;

    if (stmt->isFinalRead())
      continue;

    isl_set *domain = stmt->getDomain();
    isl_union_map *stmtBand;
    stmtBand = isl_union_map_intersect_domain(isl_union_map_copy(tiledSchedule),
					      isl_union_set_from_set(domain));
    isl_map *stmtSchedule;
    isl_union_map_foreach_map(stmtBand, getSingleMap, &stmtSchedule);
    stmt->setScattering(stmtSchedule);
    isl_union_map_free(stmtBand);
  }

  isl_union_map_free(tiledSchedule);
  isl_schedule_free(schedule);

  unsigned maxScatDims = 0;

  for (Scop::iterator SI = S.begin(), SE = S.end(); SI != SE; ++SI)
    maxScatDims = std::max((*SI)->getNumScattering(), maxScatDims);

  extendScattering(S, maxScatDims);
  return false;
}

void IslScheduleOptimizer::printScop(raw_ostream &OS) const {
}

void IslScheduleOptimizer::getAnalysisUsage(AnalysisUsage &AU) const {
  ScopPass::getAnalysisUsage(AU);
  AU.addRequired<Dependences>();
}

INITIALIZE_PASS_BEGIN(IslScheduleOptimizer, "polly-optimize-isl",
                      "Polly - Optimize schedule of SCoP", false, false)
INITIALIZE_PASS_DEPENDENCY(Dependences)
INITIALIZE_PASS_DEPENDENCY(ScopInfo)
INITIALIZE_PASS_END(IslScheduleOptimizer, "polly-optimize-isl",
                      "Polly - Optimize schedule of SCoP", false, false)

Pass* polly::createIslScheduleOptimizerPass() {
  return new IslScheduleOptimizer();
}
