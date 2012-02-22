//===- Platform/Platform.h - Platform Interface ---------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_PLATFORM_PLATFORM_H_
#define LLD_PLATFORM_PLATFORM_H_

#include <vector>

namespace lld {
class Atom;
class DefinedAtom;

/// The Platform class encapsulated plaform specific linking knowledge.
///
/// Much of what it does is driving by platform specific linker options.
class Platform {
public:
  virtual void initialize() = 0;

  /// @brief tell platform object another file has been added
  virtual void fileAdded(const File &file) = 0;

  /// @brief tell platform object another atom has been added
  virtual void atomAdded(const Atom &file) = 0;

  /// @brief give platform a chance to change each atom's scope
  virtual void adjustScope(const DefinedAtom &atom) = 0;

  /// @brief if specified atom needs alternate names, return AliasAtom(s)
  virtual bool getAliasAtoms(const Atom &atom,
                             std::vector<const DefinedAtom *>&) = 0;

  /// @brief give platform a chance to resolve platform-specific undefs
  virtual bool getPlatformAtoms(llvm::StringRef undefined,
                                std::vector<const DefinedAtom *>&) = 0;

  /// @brief resolver should remove unreferenced atoms
  virtual bool deadCodeStripping() = 0;

  /// @brief atom must be kept so should be root of dead-strip graph
  virtual bool isDeadStripRoot(const Atom &atom) = 0;

  /// @brief if target must have some atoms, denote here
  virtual bool getImplicitDeadStripRoots(std::vector<const DefinedAtom *>&) = 0;

  /// @brief return entry point for output file (e.g. "main") or NULL
  virtual llvm::StringRef entryPointName() = 0;

  /// @brief for iterating must-be-defined symbols ("main" or -u command line
  ///        option)
  typedef llvm::StringRef const *UndefinesIterator;
  virtual UndefinesIterator  initialUndefinesBegin() const = 0;
  virtual UndefinesIterator  initialUndefinesEnd() const = 0;

  /// @brief if platform wants resolvers to search libraries for overrides
  virtual bool searchArchivesToOverrideTentativeDefinitions() = 0;
  virtual bool searchSharedLibrariesToOverrideTentativeDefinitions() = 0;

  /// @brief if platform allows symbol to remain undefined (e.g. -r)
  virtual bool allowUndefinedSymbol(llvm::StringRef name) = 0;

  /// @brief for debugging dead code stripping, -why_live
  virtual bool printWhyLive(llvm::StringRef name) = 0;

  /// When core linking finds a duplicate definition, the platform
  /// can either print an error message and terminate or return with
  /// which atom the linker should use.
  virtual const Atom& handleMultipleDefinitions(const Atom& def1, 
                                                const Atom& def2) = 0;

  /// @brief print out undefined symbol error messages in platform specific way
  virtual void errorWithUndefines(const std::vector<const Atom *>& undefs,
                                  const std::vector<const Atom *>& all) = 0;

  /// When core linking finds undefined atoms from different object
  /// files that have different canBeNull values, this method is called.
  /// The useUndef2 parameter is set to which canBeNull setting the
  /// linker should use, and can be changed by this method.  Or this
  /// method can emit a warning/error about the mismatch.
  virtual void undefineCanBeNullMismatch(const UndefinedAtom& undef1,
                                         const UndefinedAtom& undef2,
                                         bool& useUndef2) = 0;

  /// When core linking finds shared library atoms from different object
  /// files that have different attribute values, this method is called.
  /// The useShlib2 parameter is set to which atom attributes the
  /// linker should use, and can be changed by this method.  Or this
  /// method can emit a warning/error about the mismatch.
  virtual void sharedLibrarylMismatch(const SharedLibraryAtom& shLib1,
                                      const SharedLibraryAtom& shLib2,
                                      bool& useShlib2) = 0;

  /// @brief last chance for platform to tweak atoms
  virtual void postResolveTweaks(std::vector<const Atom *>& all) = 0;
};

} // namespace lld

#endif // LLD_PLATFORM_PLATFORM_H_
