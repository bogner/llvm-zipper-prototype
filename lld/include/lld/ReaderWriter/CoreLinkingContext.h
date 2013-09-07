//===- lld/ReaderWriter/CoreLinkingContext.h ------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_CORE_LINKER_CONTEXT_H
#define LLD_READER_WRITER_CORE_LINKER_CONTEXT_H

#include "lld/Core/LinkingContext.h"
#include "lld/ReaderWriter/Reader.h"
#include "lld/ReaderWriter/Writer.h"

#include "llvm/Support/ErrorHandling.h"

namespace lld {

class CoreLinkingContext : public LinkingContext {
public:
  CoreLinkingContext();

  virtual bool validateImpl(raw_ostream &diagnostics);
  virtual void addPasses(PassManager &pm) const;
  virtual ErrorOr<Reference::Kind> relocKindFromString(StringRef str) const;
  virtual ErrorOr<std::string> stringFromRelocKind(Reference::Kind kind) const;

  virtual error_code
  parseFile(LinkerInput &input,
            std::vector<std::unique_ptr<File> > &result) const;

  void addPassNamed(StringRef name) { _passNames.push_back(name); }

protected:
  virtual Writer &writer() const;

private:
  mutable std::unique_ptr<Reader> _reader;
  mutable std::unique_ptr<Writer> _writer;
  std::vector<StringRef> _passNames;
};

} // end namespace lld

#endif
