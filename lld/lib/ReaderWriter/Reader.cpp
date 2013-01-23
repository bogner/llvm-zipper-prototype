//===- lib/ReaderWriter/Reader.cpp ----------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lld/ReaderWriter/Reader.h"

#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/system_error.h"

namespace lld {
Reader::~Reader() {
}

error_code Reader::readFile(StringRef path,
                            std::vector<std::unique_ptr<File>> &result) {
  OwningPtr<llvm::MemoryBuffer> opmb;
  if (error_code ec = llvm::MemoryBuffer::getFileOrSTDIN(path, opmb))
    return ec;
 
  std::unique_ptr<MemoryBuffer> mb(opmb.take());
  return this->parseFile(std::move(mb), result);
}
} // end namespace lld 
