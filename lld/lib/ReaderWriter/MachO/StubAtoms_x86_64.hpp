//===- lib/ReaderWriter/MachO/StubAtoms_x86_64.hpp ------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_MACHO_STUB_ATOMS_X86_64_H
#define LLD_READER_WRITER_MACHO_STUB_ATOMS_X86_64_H

#include "llvm/ADT/ArrayRef.h"

#include "lld/Core/DefinedAtom.h"
#include "lld/Core/SharedLibraryAtom.h"
#include "lld/Core/File.h"
#include "lld/Core/Reference.h"

#include "ReferenceKinds.h"

using llvm::makeArrayRef;
using namespace llvm::MachO;

namespace lld {
namespace mach_o {

//
// X86_64 Stub Atom created by the stubs pass.
//
class X86_64StubAtom : public SimpleDefinedAtom {
public:
  X86_64StubAtom(const File &file, const Atom &lazyPointer)
      : SimpleDefinedAtom(file) {
    this->addReference(Reference::KindNamespace::mach_o,
                       Reference::KindArch::x86_64, X86_64_RELOC_SIGNED, 2,
                       &lazyPointer, 0);
  }

  ContentType contentType() const override {
    return DefinedAtom::typeStub;
  }

  uint64_t size() const override {
    return 6;
  }

  ContentPermissions permissions() const override {
    return DefinedAtom::permR_X;
  }

  ArrayRef<uint8_t> rawContent() const override {
    static const uint8_t instructions[] =
        { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 }; // jmp *lazyPointer
    assert(sizeof(instructions) == this->size());
    return makeArrayRef(instructions);
  }
};

//
// X86_64 Stub Helper Common Atom created by the stubs pass.
//
class X86_64StubHelperCommonAtom : public SimpleDefinedAtom {
public:
  X86_64StubHelperCommonAtom(const File &file, const Atom &cache,
                             const Atom &binder)
      : SimpleDefinedAtom(file) {
    this->addReference(Reference::KindNamespace::mach_o,
                       Reference::KindArch::x86_64, X86_64_RELOC_SIGNED, 3,
                       &cache, 0);
    this->addReference(Reference::KindNamespace::mach_o,
                       Reference::KindArch::x86_64, X86_64_RELOC_SIGNED, 11,
                       &binder, 0);
  }

  ContentType contentType() const override {
    return DefinedAtom::typeStubHelper;
  }

  uint64_t size() const override {
    return 16;
  }

  ContentPermissions permissions() const override {
    return DefinedAtom::permR_X;
  }

  ArrayRef<uint8_t> rawContent() const override {
    static const uint8_t instructions[] =
        { 0x4C, 0x8D, 0x1D, 0x00, 0x00, 0x00, 0x00,   // leaq cache(%rip),%r11
          0x41, 0x53,                                 // push %r11
          0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,         // jmp *binder(%rip)
          0x90 };                                     // nop
    assert(sizeof(instructions) == this->size());
    return makeArrayRef(instructions);
  }
};

//
// X86_64 Stub Helper Atom created by the stubs pass.
//
class X86_64StubHelperAtom : public SimpleDefinedAtom {
public:
  X86_64StubHelperAtom(const File &file, const Atom &helperCommon)
      : SimpleDefinedAtom(file) {
    this->addReference(Reference::KindNamespace::mach_o,
                       Reference::KindArch::x86_64,
                       LLD_X86_64_RELOC_LAZY_IMMEDIATE, 1, this, 0);
    this->addReference(Reference::KindNamespace::mach_o,
                       Reference::KindArch::x86_64, X86_64_RELOC_SIGNED, 6,
                       &helperCommon, 0);
  }

  ContentType contentType() const override {
    return DefinedAtom::typeStubHelper;
  }

  uint64_t size() const override {
    return 10;
  }

  ContentPermissions permissions() const override {
    return DefinedAtom::permR_X;
  }

  ArrayRef<uint8_t> rawContent() const override {
    static const uint8_t instructions[] =
        { 0x68, 0x00, 0x00, 0x00, 0x00,   // pushq $lazy-info-offset
          0xE9, 0x00, 0x00, 0x00, 0x00 }; // jmp helperhelper
    assert(sizeof(instructions) == this->size());
    return makeArrayRef(instructions);
  }
};

//
// X86_64 Lazy Pointer Atom created by the stubs pass.
//
class X86_64LazyPointerAtom : public SimpleDefinedAtom {
public:
  X86_64LazyPointerAtom(const File &file, const Atom &helper,
                        const Atom &shlib)
      : SimpleDefinedAtom(file) {
    this->addReference(Reference::KindNamespace::mach_o,
                       Reference::KindArch::x86_64, X86_64_RELOC_UNSIGNED, 0,
                       &helper, 0);
    this->addReference(Reference::KindNamespace::mach_o,
                       Reference::KindArch::x86_64,
                       LLD_X86_64_RELOC_LAZY_TARGET, 0, &shlib, 0);
  }

  ContentType contentType() const override {
    return DefinedAtom::typeLazyPointer;
  }

  Alignment alignment() const override {
    return Alignment(3);
  }

  uint64_t size() const override {
    return 8;
  }

  ContentPermissions permissions() const override {
    return DefinedAtom::permRW_;
  }

  ArrayRef<uint8_t> rawContent() const override {
    static const uint8_t bytes[] =
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    return makeArrayRef(bytes);
  }
};

//
// X86_64 NonLazy (GOT) Pointer Atom created by the stubs pass.
//
class X86_64NonLazyPointerAtom : public SimpleDefinedAtom {
public:
  X86_64NonLazyPointerAtom(const File &file)
      : SimpleDefinedAtom(file) {}

  X86_64NonLazyPointerAtom(const File &file, const Atom &shlib)
      : SimpleDefinedAtom(file) {
    this->addReference(Reference::KindNamespace::mach_o,
                       Reference::KindArch::x86_64, X86_64_RELOC_UNSIGNED, 0,
                       &shlib, 0);
  }

  ContentType contentType() const override {
    return DefinedAtom::typeGOT;
  }

  Alignment alignment() const override {
    return Alignment(3);
  }

  uint64_t size() const override {
    return 8;
  }

  ContentPermissions permissions() const override {
    return DefinedAtom::permRW_;
  }

  ArrayRef<uint8_t> rawContent() const override {
    static const uint8_t bytes[] =
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    return makeArrayRef(bytes);
  }
};

} // namespace mach_o
} // namespace lld


#endif // LLD_READER_WRITER_MACHO_STUB_ATOMS_X86_64_H
