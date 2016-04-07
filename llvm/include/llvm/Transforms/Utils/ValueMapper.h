//===- ValueMapper.h - Remapping for constants and metadata -----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the MapValue interface which is used by various parts of
// the Transforms/Utils library to implement cloning and linking facilities.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_VALUEMAPPER_H
#define LLVM_TRANSFORMS_UTILS_VALUEMAPPER_H

#include "llvm/IR/ValueMap.h"

namespace llvm {

class Value;
class Instruction;
typedef ValueMap<const Value *, WeakVH> ValueToValueMapTy;

/// This is a class that can be implemented by clients to remap types when
/// cloning constants and instructions.
class ValueMapTypeRemapper {
  virtual void anchor(); // Out of line method.
public:
  virtual ~ValueMapTypeRemapper() {}

  /// The client should implement this method if they want to remap types while
  /// mapping values.
  virtual Type *remapType(Type *SrcTy) = 0;
};

/// This is a class that can be implemented by clients to materialize Values on
/// demand.
class ValueMaterializer {
  virtual void anchor(); // Out of line method.

protected:
  ~ValueMaterializer() = default;
  ValueMaterializer() = default;
  ValueMaterializer(const ValueMaterializer &) = default;
  ValueMaterializer &operator=(const ValueMaterializer &) = default;

public:
  /// The client should implement this method if they want to generate a mapped
  /// Value on demand. For example, if linking lazily.
  virtual Value *materializeDeclFor(Value *V) = 0;

  /// If the data being mapped is recursive, the above function can map just
  /// the declaration and this is called to compute the initializer.  It is
  /// called after the mapping is recorded, so it doesn't need to worry about
  /// recursion.
  virtual void materializeInitFor(GlobalValue *New, GlobalValue *Old);
};

/// These are flags that the value mapping APIs allow.
enum RemapFlags {
  RF_None = 0,

  /// If this flag is set, the remapper knows that only local values within a
  /// function (such as an instruction or argument) are mapped, not global
  /// values like functions and global metadata.
  RF_NoModuleLevelChanges = 1,

  /// If this flag is set, the remapper ignores missing function-local entries
  /// (Argument, Instruction, BasicBlock) that are not in the
  /// value map.  If it is unset, it aborts if an operand is asked to be
  /// remapped which doesn't exist in the mapping.
  ///
  /// There are no such assertions in MapValue(), whose result should be
  /// essentially unchanged by this flag.  This only changes the assertion
  /// behaviour in RemapInstruction().
  RF_IgnoreMissingLocals = 2,

  /// Instruct the remapper to move distinct metadata instead of duplicating it
  /// when there are module-level changes.
  RF_MoveDistinctMDs = 4,

  /// Any global values not in value map are mapped to null instead of mapping
  /// to self.  Illegal if RF_IgnoreMissingLocals is also set.
  RF_NullMapMissingGlobalValues = 8,
};

static inline RemapFlags operator|(RemapFlags LHS, RemapFlags RHS) {
  return RemapFlags(unsigned(LHS) | unsigned(RHS));
}

/// Look up or compute a value in the value map.
///
/// Return a mapped value for a function-local value (Argument, Instruction,
/// BasicBlock), or compute and memoize a value for a Constant.
///
///  1. If \c V is in VM, return the result.
///  2. Else if \c V can be materialized with \c Materializer, do so, memoize
///     it in \c VM, and return it.
///  3. Else if \c V is a function-local value, return nullptr.
///  4. Else if \c V is a \a GlobalValue, return \c nullptr or \c V depending
///     on \a RF_NullMapMissingGlobalValues.
///  5. Else, Compute the equivalent constant, and return it.
Value *MapValue(const Value *V, ValueToValueMapTy &VM,
                RemapFlags Flags = RF_None,
                ValueMapTypeRemapper *TypeMapper = nullptr,
                ValueMaterializer *Materializer = nullptr);

Metadata *MapMetadata(const Metadata *MD, ValueToValueMapTy &VM,
                      RemapFlags Flags = RF_None,
                      ValueMapTypeRemapper *TypeMapper = nullptr,
                      ValueMaterializer *Materializer = nullptr);

/// Version of MapMetadata with type safety for MDNode.
MDNode *MapMetadata(const MDNode *MD, ValueToValueMapTy &VM,
                    RemapFlags Flags = RF_None,
                    ValueMapTypeRemapper *TypeMapper = nullptr,
                    ValueMaterializer *Materializer = nullptr);

void RemapInstruction(Instruction *I, ValueToValueMapTy &VM,
                      RemapFlags Flags = RF_None,
                      ValueMapTypeRemapper *TypeMapper = nullptr,
                      ValueMaterializer *Materializer = nullptr);

/// Version of MapValue with type safety for Constant.
inline Constant *MapValue(const Constant *V, ValueToValueMapTy &VM,
                          RemapFlags Flags = RF_None,
                          ValueMapTypeRemapper *TypeMapper = nullptr,
                          ValueMaterializer *Materializer = nullptr) {
  return cast<Constant>(
      MapValue((const Value *)V, VM, Flags, TypeMapper, Materializer));
}

} // End llvm namespace

#endif
