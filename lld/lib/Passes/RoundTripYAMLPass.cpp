//===--Passes/RoundTripYAMLPass.cpp - Write YAML file/Read it back---------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#define DEBUG_TYPE "RoundTripYAMLPass"

#include "lld/Core/Instrumentation.h"
#include "lld/Passes/RoundTripYAMLPass.h"
#include "lld/ReaderWriter/Simple.h"
#include "lld/ReaderWriter/Writer.h"

#include "llvm/Support/Path.h"

// Skip YAML files larger than this to avoid OOM error. The YAML reader consumes
// excessively large amount of memory when parsing a large file.
// TODO: Fix the YAML reader to reduce memory footprint.
static const size_t MAX_YAML_FILE_SIZE = 50 * 1024 * 1024;

using namespace lld;

/// Perform the actual pass
void RoundTripYAMLPass::perform(std::unique_ptr<MutableFile> &mergedFile) {
  ScopedTask task(getDefaultDomain(), "RoundTripYAMLPass");
  std::unique_ptr<Writer> yamlWriter = createWriterYAML(_context);
  SmallString<128> tmpYAMLFile;
  // Separate the directory from the filename
  StringRef outFile = llvm::sys::path::filename(_context.outputPath());
  if (llvm::sys::fs::createTemporaryFile(outFile, "yaml", tmpYAMLFile))
    return;

  // The file that is written would be kept around if there is a problem
  // writing to the file or when reading atoms back from the file.
  yamlWriter->writeFile(*mergedFile, tmpYAMLFile.str());
  OwningPtr<MemoryBuffer> buff;
  if (MemoryBuffer::getFile(tmpYAMLFile.str(), buff))
    return;

  if (buff->getBufferSize() < MAX_YAML_FILE_SIZE) {
    std::unique_ptr<MemoryBuffer> mb(buff.take());
    _context.getYAMLReader().parseFile(mb, _yamlFile);
    mergedFile.reset(new FileToMutable(_context, *_yamlFile[0].get()));
  }

  llvm::sys::fs::remove(tmpYAMLFile.str());
}
