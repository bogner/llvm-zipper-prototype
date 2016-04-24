//===-- llvm/ModuleSummaryIndex.h - Module Summary Index --------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// ModuleSummaryIndex.h This file contains the declarations the classes that
///  hold the module index and summary for function importing.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_MODULESUMMARYINDEX_H
#define LLVM_IR_MODULESUMMARYINDEX_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/Module.h"

#include <array>

namespace llvm {

/// \brief Class to accumulate and hold information about a callee.
struct CalleeInfo {
  /// The static number of callsites calling corresponding function.
  unsigned CallsiteCount;
  /// The cumulative profile count of calls to corresponding function
  /// (if using PGO, otherwise 0).
  uint64_t ProfileCount;
  CalleeInfo() : CallsiteCount(0), ProfileCount(0) {}
  CalleeInfo(unsigned CallsiteCount, uint64_t ProfileCount)
      : CallsiteCount(CallsiteCount), ProfileCount(ProfileCount) {}
  CalleeInfo &operator+=(uint64_t RHSProfileCount) {
    CallsiteCount++;
    ProfileCount += RHSProfileCount;
    return *this;
  }
};

/// Struct to hold value either by GUID or Value*, depending on whether this
/// is a combined or per-module index, respectively.
struct ValueInfo {
  /// The value representation used in this instance.
  enum ValueInfoKind {
    VI_GUID,
    VI_Value,
  };

  /// Union of the two possible value types.
  union ValueUnion {
    GlobalValue::GUID Id;
    const Value *V;
    ValueUnion(GlobalValue::GUID Id) : Id(Id) {}
    ValueUnion(const Value *V) : V(V) {}
  };

  /// The value being represented.
  ValueUnion TheValue;
  /// The value representation.
  ValueInfoKind Kind;
  /// Constructor for a GUID value
  ValueInfo(GlobalValue::GUID Id = 0) : TheValue(Id), Kind(VI_GUID) {}
  /// Constructor for a Value* value
  ValueInfo(const Value *V) : TheValue(V), Kind(VI_Value) {}
  /// Accessor for GUID value
  GlobalValue::GUID getGUID() const {
    assert(Kind == VI_GUID && "Not a GUID type");
    return TheValue.Id;
  }
  /// Accessor for Value* value
  const Value *getValue() const {
    assert(Kind == VI_Value && "Not a Value type");
    return TheValue.V;
  }
};

/// \brief Function and variable summary information to aid decisions and
/// implementation of importing.
///
/// This is a separate class from GlobalValueInfo to enable lazy reading of this
/// summary information from the combined index file during imporing.
class GlobalValueSummary {
public:
  /// \brief Sububclass discriminator (for dyn_cast<> et al.)
  enum SummaryKind { AliasKind, FunctionKind, GlobalVarKind };

  /// Group flags (Linkage, hasSection, isOptSize, etc.) as a bitfield.
  struct GVFlags {
    /// \brief The linkage type of the associated global value.
    ///
    /// One use is to flag values that have local linkage types and need to
    /// have module identifier appended before placing into the combined
    /// index, to disambiguate from other values with the same name.
    /// In the future this will be used to update and optimize linkage
    /// types based on global summary-based analysis.
    GlobalValue::LinkageTypes Linkage : 4;

    /// Indicate if the global value is located in a specific section.
    unsigned HasSection : 1;

    /// Convenience Constructors
    explicit GVFlags(GlobalValue::LinkageTypes Linkage, bool HasSection)
        : Linkage(Linkage), HasSection(HasSection) {}
    GVFlags(const GlobalValue &GV)
        : Linkage(GV.getLinkage()), HasSection(GV.hasSection()) {}
  };

private:
  /// Kind of summary for use in dyn_cast<> et al.
  SummaryKind Kind;

  /// This is the hash of the name of the symbol in the original file. It is
  /// identical to the GUID for global symbols, but differs for local since the
  /// GUID includes the module level id in the hash.
  GlobalValue::GUID OriginalName;

  /// \brief Path of module IR containing value's definition, used to locate
  /// module during importing.
  ///
  /// This is only used during parsing of the combined index, or when
  /// parsing the per-module index for creation of the combined summary index,
  /// not during writing of the per-module index which doesn't contain a
  /// module path string table.
  StringRef ModulePath;

  GVFlags Flags;

  /// List of values referenced by this global value's definition
  /// (either by the initializer of a global variable, or referenced
  /// from within a function). This does not include functions called, which
  /// are listed in the derived FunctionSummary object.
  std::vector<ValueInfo> RefEdgeList;

protected:
  /// GlobalValueSummary constructor.
  GlobalValueSummary(SummaryKind K, GVFlags Flags) : Kind(K), Flags(Flags) {}

public:
  virtual ~GlobalValueSummary() = default;

  /// Returns the hash of the original name, it is identical to the GUID for
  /// externally visible symbols, but not for local ones.
  GlobalValue::GUID getOriginalName() { return OriginalName; }

  /// Initialize the original name hash in this summary.
  void setOriginalName(GlobalValue::GUID Name) { OriginalName = Name; }

  /// Which kind of summary subclass this is.
  SummaryKind getSummaryKind() const { return Kind; }

  /// Set the path to the module containing this function, for use in
  /// the combined index.
  void setModulePath(StringRef ModPath) { ModulePath = ModPath; }

  /// Get the path to the module containing this function.
  StringRef modulePath() const { return ModulePath; }

  /// Get the flags for this GlobalValue (see \p struct GVFlags).
  GVFlags flags() { return Flags; }

  /// Return linkage type recorded for this global value.
  GlobalValue::LinkageTypes linkage() const { return Flags.Linkage; }

  /// Return true if this global value is located in a specific section.
  bool hasSection() const { return Flags.HasSection; }

  /// Record a reference from this global value to the global value identified
  /// by \p RefGUID.
  void addRefEdge(GlobalValue::GUID RefGUID) { RefEdgeList.push_back(RefGUID); }

  /// Record a reference from this global value to the global value identified
  /// by \p RefV.
  void addRefEdge(const Value *RefV) { RefEdgeList.push_back(RefV); }

  /// Record a reference from this global value to each global value identified
  /// in \p RefEdges.
  void addRefEdges(DenseSet<const Value *> &RefEdges) {
    for (auto &RI : RefEdges)
      addRefEdge(RI);
  }

  /// Return the list of values referenced by this global value definition.
  std::vector<ValueInfo> &refs() { return RefEdgeList; }
  const std::vector<ValueInfo> &refs() const { return RefEdgeList; }
};

/// \brief Alias summary information.
class AliasSummary : public GlobalValueSummary {
  GlobalValueSummary *AliaseeSummary;

public:
  /// Summary constructors.
  AliasSummary(GVFlags Flags) : GlobalValueSummary(AliasKind, Flags) {}

  /// Check if this is an alias summary.
  static bool classof(const GlobalValueSummary *GVS) {
    return GVS->getSummaryKind() == AliasKind;
  }

  void setAliasee(GlobalValueSummary *Aliasee) { AliaseeSummary = Aliasee; }

  const GlobalValueSummary &getAliasee() const {
    return const_cast<AliasSummary *>(this)->getAliasee();
  }

  GlobalValueSummary &getAliasee() {
    assert(AliaseeSummary && "Unexpected missing aliasee summary");
    return *AliaseeSummary;
  }
};

/// \brief Function summary information to aid decisions and implementation of
/// importing.
class FunctionSummary : public GlobalValueSummary {
public:
  /// <CalleeValueInfo, CalleeInfo> call edge pair.
  typedef std::pair<ValueInfo, CalleeInfo> EdgeTy;

private:
  /// Number of instructions (ignoring debug instructions, e.g.) computed
  /// during the initial compile step when the summary index is first built.
  unsigned InstCount;

  /// List of <CalleeValueInfo, CalleeInfo> call edge pairs from this function.
  std::vector<EdgeTy> CallGraphEdgeList;

public:
  /// Summary constructors.
  FunctionSummary(GVFlags Flags, unsigned NumInsts)
      : GlobalValueSummary(FunctionKind, Flags), InstCount(NumInsts) {}

  /// Check if this is a function summary.
  static bool classof(const GlobalValueSummary *GVS) {
    return GVS->getSummaryKind() == FunctionKind;
  }

  /// Get the instruction count recorded for this function.
  unsigned instCount() const { return InstCount; }

  /// Record a call graph edge from this function to the function identified
  /// by \p CalleeGUID, with \p CalleeInfo including the cumulative profile
  /// count (across all calls from this function) or 0 if no PGO.
  void addCallGraphEdge(GlobalValue::GUID CalleeGUID, CalleeInfo Info) {
    CallGraphEdgeList.push_back(std::make_pair(CalleeGUID, Info));
  }

  /// Record a call graph edge from this function to the function identified
  /// by \p CalleeV, with \p CalleeInfo including the cumulative profile
  /// count (across all calls from this function) or 0 if no PGO.
  void addCallGraphEdge(const Value *CalleeV, CalleeInfo Info) {
    CallGraphEdgeList.push_back(std::make_pair(CalleeV, Info));
  }

  /// Record a call graph edge from this function to each function recorded
  /// in \p CallGraphEdges.
  void addCallGraphEdges(DenseMap<const Value *, CalleeInfo> &CallGraphEdges) {
    for (auto &EI : CallGraphEdges)
      addCallGraphEdge(EI.first, EI.second);
  }

  /// Return the list of <CalleeValueInfo, CalleeInfo> pairs.
  std::vector<EdgeTy> &calls() { return CallGraphEdgeList; }
  const std::vector<EdgeTy> &calls() const { return CallGraphEdgeList; }
};

/// \brief Global variable summary information to aid decisions and
/// implementation of importing.
///
/// Currently this doesn't add anything to the base \p GlobalValueSummary,
/// but is a placeholder as additional info may be added to the summary
/// for variables.
class GlobalVarSummary : public GlobalValueSummary {

public:
  /// Summary constructors.
  GlobalVarSummary(GVFlags Flags) : GlobalValueSummary(GlobalVarKind, Flags) {}

  /// Check if this is a global variable summary.
  static bool classof(const GlobalValueSummary *GVS) {
    return GVS->getSummaryKind() == GlobalVarKind;
  }
};

/// \brief Class to hold pointer to summary object and information required
/// for parsing or writing it.
class GlobalValueInfo {
private:
  /// Summary information used to help make ThinLTO importing decisions.
  std::unique_ptr<GlobalValueSummary> Summary;

  /// \brief The bitcode offset corresponding to either an associated
  /// function's function body record, or to an associated summary record,
  /// depending on whether this is a per-module or combined index.
  ///
  /// This bitcode offset is written to or read from the associated
  /// \a ValueSymbolTable entry for a function.
  /// For the per-module index this holds the bitcode offset of a
  /// function's body record within bitcode module block in its module,
  /// although this field is currently only used when writing the VST
  /// (it is set to 0 and also unused when this is a global variable).
  /// For the combined index this holds the offset of the corresponding
  /// summary record, to enable associating the combined index
  /// VST records with the summary records.
  uint64_t BitcodeIndex;

public:
  GlobalValueInfo(uint64_t Offset = 0,
                  std::unique_ptr<GlobalValueSummary> Summary = nullptr)
      : Summary(std::move(Summary)), BitcodeIndex(Offset) {}

  /// Record the summary information parsed out of the summary block during
  /// parsing or combined index creation.
  void setSummary(std::unique_ptr<GlobalValueSummary> GVSummary) {
    Summary = std::move(GVSummary);
  }

  /// Get the summary recorded for this global value.
  GlobalValueSummary *summary() const { return Summary.get(); }

  /// Get the bitcode index recorded for this value symbol table entry.
  uint64_t bitcodeIndex() const { return BitcodeIndex; }

  /// Set the bitcode index recorded for this value symbol table entry.
  void setBitcodeIndex(uint64_t Offset) { BitcodeIndex = Offset; }
};

/// 160 bits SHA1
typedef std::array<uint32_t, 5> ModuleHash;

/// List of global value info structures for a particular value held
/// in the GlobalValueMap. Requires a vector in the case of multiple
/// COMDAT values of the same name.
typedef std::vector<std::unique_ptr<GlobalValueInfo>> GlobalValueInfoList;

/// Map from global value GUID to corresponding info structures.
/// Use a std::map rather than a DenseMap since it will likely incur
/// less overhead, as the value type is not very small and the size
/// of the map is unknown, resulting in inefficiencies due to repeated
/// insertions and resizing.
typedef std::map<GlobalValue::GUID, GlobalValueInfoList> GlobalValueInfoMapTy;

/// Type used for iterating through the global value info map.
typedef GlobalValueInfoMapTy::const_iterator const_globalvalueinfo_iterator;
typedef GlobalValueInfoMapTy::iterator globalvalueinfo_iterator;

/// String table to hold/own module path strings, which additionally holds the
/// module ID assigned to each module during the plugin step, as well as a hash
/// of the module. The StringMap makes a copy of and owns inserted strings.
typedef StringMap<std::pair<uint64_t, ModuleHash>> ModulePathStringTableTy;

/// Class to hold module path string table and global value map,
/// and encapsulate methods for operating on them.
class ModuleSummaryIndex {
private:
  /// Map from value name to list of information instances for values of that
  /// name (may be duplicates in the COMDAT case, e.g.).
  GlobalValueInfoMapTy GlobalValueMap;

  /// Holds strings for combined index, mapping to the corresponding module ID.
  ModulePathStringTableTy ModulePathStringTable;

public:
  ModuleSummaryIndex() = default;

  // Disable the copy constructor and assignment operators, so
  // no unexpected copying/moving occurs.
  ModuleSummaryIndex(const ModuleSummaryIndex &) = delete;
  void operator=(const ModuleSummaryIndex &) = delete;

  globalvalueinfo_iterator begin() { return GlobalValueMap.begin(); }
  const_globalvalueinfo_iterator begin() const {
    return GlobalValueMap.begin();
  }
  globalvalueinfo_iterator end() { return GlobalValueMap.end(); }
  const_globalvalueinfo_iterator end() const { return GlobalValueMap.end(); }

  /// Get the list of global value info objects for a given value name.
  const GlobalValueInfoList &getGlobalValueInfoList(StringRef ValueName) {
    return GlobalValueMap[GlobalValue::getGUID(ValueName)];
  }

  /// Get the list of global value info objects for a given value name.
  const const_globalvalueinfo_iterator
  findGlobalValueInfoList(StringRef ValueName) const {
    return GlobalValueMap.find(GlobalValue::getGUID(ValueName));
  }

  /// Get the list of global value info objects for a given value GUID.
  const const_globalvalueinfo_iterator
  findGlobalValueInfoList(GlobalValue::GUID ValueGUID) const {
    return GlobalValueMap.find(ValueGUID);
  }

  /// Add a global value info for a value of the given name.
  void addGlobalValueInfo(StringRef ValueName,
                          std::unique_ptr<GlobalValueInfo> Info) {
    GlobalValueMap[GlobalValue::getGUID(ValueName)].push_back(std::move(Info));
  }

  /// Add a global value info for a value of the given GUID.
  void addGlobalValueInfo(GlobalValue::GUID ValueGUID,
                          std::unique_ptr<GlobalValueInfo> Info) {
    GlobalValueMap[ValueGUID].push_back(std::move(Info));
  }

  /// Returns the first GlobalValueInfo for \p GV, asserting that there
  /// is only one if \p PerModuleIndex.
  GlobalValueInfo *getGlobalValueInfo(const GlobalValue &GV,
                                      bool PerModuleIndex = true) const {
    assert(GV.hasName() && "Can't get GlobalValueInfo for GV with no name");
    return getGlobalValueInfo(GlobalValue::getGUID(GV.getName()),
                              PerModuleIndex);
  }

  /// Returns the first GlobalValueInfo for \p ValueGUID, asserting that there
  /// is only one if \p PerModuleIndex.
  GlobalValueInfo *getGlobalValueInfo(GlobalValue::GUID ValueGUID,
                                      bool PerModuleIndex = true) const;

  /// Table of modules, containing module hash and id.
  const StringMap<std::pair<uint64_t, ModuleHash>> &modulePaths() const {
    return ModulePathStringTable;
  }

  /// Table of modules, containing hash and id.
  StringMap<std::pair<uint64_t, ModuleHash>> &modulePaths() {
    return ModulePathStringTable;
  }

  /// Get the module ID recorded for the given module path.
  uint64_t getModuleId(const StringRef ModPath) const {
    return ModulePathStringTable.lookup(ModPath).first;
  }

  /// Get the module SHA1 hash recorded for the given module path.
  const ModuleHash &getModuleHash(const StringRef ModPath) const {
    auto It = ModulePathStringTable.find(ModPath);
    assert(It != ModulePathStringTable.end() && "Module not registered");
    return It->second.second;
  }

  /// Add the given per-module index into this module index/summary,
  /// assigning it the given module ID. Each module merged in should have
  /// a unique ID, necessary for consistent renaming of promoted
  /// static (local) variables.
  void mergeFrom(std::unique_ptr<ModuleSummaryIndex> Other,
                 uint64_t NextModuleId);

  /// Convenience method for creating a promoted global name
  /// for the given value name of a local, and its original module's ID.
  static std::string getGlobalNameForLocal(StringRef Name, ModuleHash ModHash) {
    SmallString<256> NewName(Name);
    NewName += ".llvm.";
    NewName += utohexstr(ModHash[0]); // Take the first 32 bits
    return NewName.str();
  }

  /// Add a new module path with the given \p Hash, mapped to the given \p
  /// ModID, and return an iterator to the entry in the index.
  ModulePathStringTableTy::iterator
  addModulePath(StringRef ModPath, uint64_t ModId,
                ModuleHash Hash = ModuleHash{{0}}) {
    return ModulePathStringTable.insert(std::make_pair(
                                            ModPath,
                                            std::make_pair(ModId, Hash))).first;
  }

  /// Check if the given Module has any functions available for exporting
  /// in the index. We consider any module present in the ModulePathStringTable
  /// to have exported functions.
  bool hasExportedFunctions(const Module &M) const {
    return ModulePathStringTable.count(M.getModuleIdentifier());
  }

  /// Remove entries in the GlobalValueMap that have empty summaries due to the
  /// eager nature of map entry creation during VST parsing. These would
  /// also be suppressed during combined index generation in mergeFrom(),
  /// but if there was only one module or this was the first module we might
  /// not invoke mergeFrom.
  void removeEmptySummaryEntries();

  /// Collect for the given module the list of function it defines
  /// (GUID -> Summary).
  void collectDefinedFunctionsForModule(
      StringRef ModulePath,
      std::map<GlobalValue::GUID, GlobalValueSummary *> &FunctionInfoMap) const;

  /// Collect for each module the list of Summaries it defines (GUID ->
  /// Summary).
  void collectDefinedGVSummariesPerModule(
      StringMap<std::map<GlobalValue::GUID, GlobalValueSummary *>> &
          ModuleToDefinedGVSummaries) const;
};

} // End llvm namespace

#endif
