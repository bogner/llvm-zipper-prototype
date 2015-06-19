//===- lib/ReaderWriter/PECOFF/LoadConfigPass.cpp -------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// A Load Configuration is a data structure for x86 containing an address of the
// SEH handler table. The Data Directory in the file header points to a load
// configuration. Technically that indirection is not needed but exists for
// historical reasons.
//
// If the file being handled has .sxdata section containing SEH handler table,
// this pass will create a Load Configuration atom.
//
//===----------------------------------------------------------------------===//

#include "Pass.h"
#include "LoadConfigPass.h"
#include "lld/Core/File.h"
#include "lld/Core/Pass.h"
#include "lld/Core/Simple.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Path.h"
#include <climits>
#include <ctime>
#include <utility>

using llvm::object::coff_load_configuration32;

namespace lld {
namespace pecoff {
namespace loadcfg {

LoadConfigAtom::LoadConfigAtom(VirtualFile &file, const DefinedAtom *sxdata,
                               int count)
    : COFFLinkerInternalAtom(
          file, file.getNextOrdinal(),
          std::vector<uint8_t>(sizeof(coff_load_configuration32))) {
  addDir32Reloc(
      this, sxdata, llvm::COFF::IMAGE_FILE_MACHINE_I386,
      offsetof(llvm::object::coff_load_configuration32, SEHandlerTable));
  auto *data = getContents<llvm::object::coff_load_configuration32>();
  data->SEHandlerCount = count;
}

} // namespace loadcfg

std::error_code LoadConfigPass::perform(SimpleFile &file) {
  if (_ctx.noSEH())
    return std::error_code();

  // Find the first atom in .sxdata section.
  const DefinedAtom *sxdata = nullptr;
  int sectionSize = 0;
  for (const DefinedAtom *atom : file.defined()) {
    if (atom->customSectionName() == ".sxdata") {
      if (!sxdata)
        sxdata = atom;
      sectionSize += sxdata->size();
    }
  }
  if (!sxdata)
    return std::error_code();

  auto *loadcfg = new (_alloc)
      loadcfg::LoadConfigAtom(_file, sxdata, sectionSize / sizeof(uint32_t));
  file.addAtom(*loadcfg);

  return std::error_code();
}

} // namespace pecoff
} // namespace lld
