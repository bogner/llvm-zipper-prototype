//===- Core/References.h - A Reference to Another Atom --------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CORE_REFERENCES_H_
#define LLD_CORE_REFERENCES_H_

#include "llvm/Support/DataTypes.h"

namespace lld {

///
/// The linker has a Graph Theory model of linking. An object file is seen
/// as a set of Atoms with References to other Atoms.  Each Atom is a node
/// and each Reference is an edge. 
///
/// For example if a function contains a call site to "malloc" 40 bytes into 
/// the Atom, then the function Atom will have a Reference of: offsetInAtom=40,
/// kind=callsite, target=malloc, addend=0.
///
/// Besides supporting traditional "relocations", References are also used
/// grouping atoms (group comdat), forcing layout (one atom must follow 
/// another), marking data-in-code (jump tables or ARM constants), etc.
///
class Reference {
public:
  /// The meaning of positive kind values is architecture specific.  
  /// Negative kind values are architecture independent.
  typedef int32_t Kind;

  // A value to be added to the value of a target
  typedef int64_t Addend;

  /// What sort of reference this is. 
  virtual Kind kind() const = 0;
  
  /// During linking, some optimizations may change the code gen and
  /// hence the reference kind.
  virtual void setKind(Kind) = 0;
  
  /// If the reference is a fixup in the Atom, then this returns the 
  /// byte offset into the Atom's content to do the fix up.
  virtual uint64_t offsetInAtom() const = 0;
  
  /// If the reference is an edge to another Atom, then this returns the
  /// other Atom.  Otherwise, it returns NULL.
  virtual const class Atom * target() const = 0;
  
  /// During linking, the linker may merge graphs which coalesces some nodes
  /// (i.e. Atoms).  To switch the target of a reference, this method is called.
  virtual void setTarget(const class Atom *) = 0;
  
  /// Some relocations require a symbol and a value (e.g. foo + 4).
  virtual Addend addend() const = 0;
  
protected:
  /// Atom is an abstract base class.  Only subclasses can access constructor.
  Reference() {}
  
  /// The memory for Reference objects is always managed by the owning File
  /// object.  Therefore, no one but the owning File object should call
  /// delete on an Reference.  In fact, some File objects may bulk allocate
  /// an array of References, so they cannot be individually deleted by anyone.
  virtual ~Reference() {}
};


} // namespace lld

#endif // LLD_CORE_REFERENCES_H_
