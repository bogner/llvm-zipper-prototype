//===- lld/Driver/WinLinkModuleDef.h --------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Windows module definition file parser.
///
//===----------------------------------------------------------------------===//

#ifndef LLD_DRIVER_WIN_LINK_MODULE_DEF_H
#define LLD_DRIVER_WIN_LINK_MODULE_DEF_H

#include "lld/Core/LLVM.h"
#include "lld/ReaderWriter/PECOFFLinkingContext.h"
#include "llvm/ADT/Optional.h"
#include "llvm/Support/Allocator.h"

namespace lld {
namespace moduledef {

enum class Kind {
  unknown,
  eof,
  identifier,
  comma,
  equal,
  kw_data,
  kw_exports,
  kw_heapsize,
  kw_noname,
};

class Token {
public:
  Token() : _kind(Kind::unknown) {}
  Token(Kind kind, StringRef range) : _kind(kind), _range(range) {}

  Kind _kind;
  StringRef _range;
};

class Lexer {
public:
  explicit Lexer(std::unique_ptr<MemoryBuffer> mb) : _buffer(mb->getBuffer()) {
    _sourceManager.AddNewSourceBuffer(mb.release(), llvm::SMLoc());
  }

  Token lex();
  const llvm::SourceMgr &getSourceMgr() const { return _sourceManager; }

private:
  StringRef _buffer;
  llvm::SourceMgr _sourceManager;
};

class Directive {
public:
  enum class Kind { exports, heapsize };

  Kind getKind() const { return _kind; }
  virtual ~Directive() {}

protected:
  explicit Directive(Kind k) : _kind(k) {}

private:
  Kind _kind;
};

class Exports : public Directive {
public:
  explicit Exports(const std::vector<PECOFFLinkingContext::ExportDesc> &exports)
      : Directive(Kind::exports), _exports(exports) {}

  static bool classof(const Directive *dir) {
    return dir->getKind() == Kind::exports;
  }

  const std::vector<PECOFFLinkingContext::ExportDesc> &getExports() const {
    return _exports;
  }

private:
  const std::vector<PECOFFLinkingContext::ExportDesc> _exports;
};

class Heapsize : public Directive {
public:
  explicit Heapsize(uint64_t reserve, uint64_t commit)
      : Directive(Kind::heapsize), _reserve(reserve), _commit(commit) {}

  static bool classof(const Directive *dir) {
    return dir->getKind() == Kind::heapsize;
  }

  uint64_t getReserve() const { return _reserve; }
  uint64_t getCommit() const { return _commit; }

private:
  const uint64_t _reserve;
  const uint64_t _commit;
};

class Parser {
public:
  explicit Parser(Lexer &lex, llvm::BumpPtrAllocator &alloc)
      : _lex(lex), _alloc(alloc) {}

  llvm::Optional<Directive *> parse();

private:
  void consumeToken();
  bool consumeTokenAsInt(uint64_t &result);
  void ungetToken();
  void error(const Token &tok, Twine msg);

  bool parseExport(PECOFFLinkingContext::ExportDesc &result);
  bool parseHeapsize(uint64_t &reserve, uint64_t &commit);

  Lexer &_lex;
  llvm::BumpPtrAllocator &_alloc;
  Token _tok;
  std::vector<Token> _tokBuf;
};
}
}

#endif
