//===- SyntheticSection.h ---------------------------------------*- C++ -*-===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_SYNTHETIC_SECTION_H
#define LLD_ELF_SYNTHETIC_SECTION_H

#include "InputSection.h"

namespace lld {
namespace elf {

// .interp section.
template <class ELFT> class InterpSection final : public InputSection<ELFT> {
public:
  InterpSection();
};

// .note.gnu.build-id section.
template <class ELFT> class BuildIdSection : public InputSection<ELFT> {
public:
  void writeTo(uint8_t *Buf) override;
  size_t getSize() const override { return 16 + HashSize; }
  virtual void writeBuildId(llvm::MutableArrayRef<uint8_t> Buf) = 0;
  virtual ~BuildIdSection() = default;

  uint8_t *getOutputLoc(uint8_t *Start) const;

protected:
  BuildIdSection(size_t HashSize);

  void
  computeHash(llvm::MutableArrayRef<uint8_t> Buf,
              std::function<void(ArrayRef<uint8_t> Arr, uint8_t *Hash)> Hash);

  size_t HashSize;
};

template <class ELFT>
class BuildIdFastHash final : public BuildIdSection<ELFT> {
public:
  BuildIdFastHash() : BuildIdSection<ELFT>(8) {}
  void writeBuildId(llvm::MutableArrayRef<uint8_t> Buf) override;
};

template <class ELFT> class BuildIdMd5 final : public BuildIdSection<ELFT> {
public:
  BuildIdMd5() : BuildIdSection<ELFT>(16) {}
  void writeBuildId(llvm::MutableArrayRef<uint8_t> Buf) override;
};

template <class ELFT> class BuildIdSha1 final : public BuildIdSection<ELFT> {
public:
  BuildIdSha1() : BuildIdSection<ELFT>(20) {}
  void writeBuildId(llvm::MutableArrayRef<uint8_t> Buf) override;
};

template <class ELFT> class BuildIdUuid final : public BuildIdSection<ELFT> {
public:
  BuildIdUuid() : BuildIdSection<ELFT>(16) {}
  void writeBuildId(llvm::MutableArrayRef<uint8_t> Buf) override;
};

template <class ELFT>
class BuildIdHexstring final : public BuildIdSection<ELFT> {
public:
  BuildIdHexstring();
  void writeBuildId(llvm::MutableArrayRef<uint8_t>) override;
};

template <class ELFT> InputSection<ELFT> *createCommonSection();

// Linker generated sections which can be used as inputs.
template <class ELFT> struct In {
  static BuildIdSection<ELFT> *BuildId;
  static InputSection<ELFT> *Common;
  static InterpSection<ELFT> *Interp;
};

template <class ELFT> BuildIdSection<ELFT> *In<ELFT>::BuildId;
template <class ELFT> InputSection<ELFT> *In<ELFT>::Common;
template <class ELFT> InterpSection<ELFT> *In<ELFT>::Interp;

} // namespace elf
} // namespace lld

#endif
