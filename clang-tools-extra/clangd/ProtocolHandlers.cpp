//===--- ProtocolHandlers.cpp - LSP callbacks -----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "ProtocolHandlers.h"
#include "DocumentStore.h"
#include "clang/Format/Format.h"
using namespace clang;
using namespace clangd;

void TextDocumentDidOpenHandler::handleNotification(
    llvm::yaml::MappingNode *Params) {
  auto DOTDP = DidOpenTextDocumentParams::parse(Params);
  if (!DOTDP) {
    Logs << "Failed to decode DidOpenTextDocumentParams!\n";
    return;
  }
  Store.addDocument(DOTDP->textDocument.uri, DOTDP->textDocument.text);
}

void TextDocumentDidChangeHandler::handleNotification(
    llvm::yaml::MappingNode *Params) {
  auto DCTDP = DidChangeTextDocumentParams::parse(Params);
  if (!DCTDP || DCTDP->contentChanges.size() != 1) {
    Logs << "Failed to decode DidChangeTextDocumentParams!\n";
    return;
  }
  // We only support full syncing right now.
  Store.addDocument(DCTDP->textDocument.uri, DCTDP->contentChanges[0].text);
}

/// Turn a [line, column] pair into an offset in Code.
static size_t positionToOffset(StringRef Code, Position P) {
  size_t Offset = 0;
  for (int I = 0; I != P.line; ++I) {
    // FIXME: \r\n
    // FIXME: UTF-8
    size_t F = Code.find('\n', Offset);
    if (F == StringRef::npos)
      return 0; // FIXME: Is this reasonable?
    Offset = F + 1;
  }
  return (Offset == 0 ? 0 : (Offset - 1)) + P.character;
}

/// Turn an offset in Code into a [line, column] pair.
static Position offsetToPosition(StringRef Code, size_t Offset) {
  StringRef JustBefore = Code.substr(0, Offset);
  // FIXME: \r\n
  // FIXME: UTF-8
  int Lines = JustBefore.count('\n');
  int Cols = JustBefore.size() - JustBefore.rfind('\n') - 1;
  return {Lines, Cols};
}

static std::string formatCode(StringRef Code, StringRef Filename,
                              ArrayRef<tooling::Range> Ranges, StringRef ID) {
  // Call clang-format.
  // FIXME: Don't ignore style.
  format::FormatStyle Style = format::getLLVMStyle();
  tooling::Replacements Replacements =
      format::reformat(Style, Code, Ranges, Filename);

  // Now turn the replacements into the format specified by the Language Server
  // Protocol. Fuse them into one big JSON array.
  std::string Edits;
  for (auto &R : Replacements) {
    Range ReplacementRange = {
        offsetToPosition(Code, R.getOffset()),
        offsetToPosition(Code, R.getOffset() + R.getLength())};
    TextEdit TE = {ReplacementRange, R.getReplacementText()};
    Edits += TextEdit::unparse(TE);
    Edits += ',';
  }
  if (!Edits.empty())
    Edits.pop_back();

  return R"({"jsonrpc":"2.0","id":)" + ID.str() +
         R"(,"result":[)" + Edits + R"(]})";
}

void TextDocumentRangeFormattingHandler::handleMethod(
    llvm::yaml::MappingNode *Params, StringRef ID) {
  auto DRFP = DocumentRangeFormattingParams::parse(Params);
  if (!DRFP) {
    Logs << "Failed to decode DocumentRangeFormattingParams!\n";
    return;
  }

  StringRef Code = Store.getDocument(DRFP->textDocument.uri);

  size_t Begin = positionToOffset(Code, DRFP->range.start);
  size_t Len = positionToOffset(Code, DRFP->range.end) - Begin;

  writeMessage(formatCode(Code, DRFP->textDocument.uri,
                          {clang::tooling::Range(Begin, Len)}, ID));
}

void TextDocumentFormattingHandler::handleMethod(
    llvm::yaml::MappingNode *Params, StringRef ID) {
  auto DFP = DocumentFormattingParams::parse(Params);
  if (!DFP) {
    Logs << "Failed to decode DocumentFormattingParams!\n";
    return;
  }

  // Format everything.
  StringRef Code = Store.getDocument(DFP->textDocument.uri);
  writeMessage(formatCode(Code, DFP->textDocument.uri,
                          {clang::tooling::Range(0, Code.size())}, ID));
}
