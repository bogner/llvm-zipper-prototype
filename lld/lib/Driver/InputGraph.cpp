//===- lib/Driver/InputGraph.cpp ------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include "lld/Driver/InputGraph.h"

using namespace lld;

namespace {
bool sortInputElements(const std::unique_ptr<InputElement> &a,
                       const std::unique_ptr<InputElement> &b) {
  return a->getOrdinal() < b->getOrdinal();
}
}

bool InputGraph::addInputElement(std::unique_ptr<InputElement> ie) {
  switch (ie->kind()) {
  case InputElement::Kind::Control:
    ++_numElements;
    break;
  case InputElement::Kind::File:
    ++_numElements;
    ++_numFiles;
    break;
  }
  _inputArgs.push_back(std::move(ie));
  return true;
}

bool InputGraph::assignOrdinals() {
  for (auto &ie : _inputArgs)
    ie->setOrdinal(++_ordinal);
  return true;
}

void InputGraph::doPostProcess() {
  std::stable_sort(_inputArgs.begin(), _inputArgs.end(), sortInputElements);
}

bool InputGraph::validate() {
  for (auto &ie : _inputArgs)
    if (!ie->validate())
      return false;
  return true;
}

bool InputGraph::dump(raw_ostream &diagnostics) {
  for (auto &ie : _inputArgs)
    if (!ie->dump(diagnostics))
      return false;
  return true;
}
