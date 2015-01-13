//===- lib/Core/InputGraph.cpp --------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lld/Core/InputGraph.h"
#include "lld/Core/Resolver.h"
#include <memory>

using namespace lld;

InputGraph::~InputGraph() { }

File *InputGraph::getNextFile() {
  for (;;) {
    if (_index >= _inputArgs.size())
      return nullptr;
    if (FileNode *node = dyn_cast<FileNode>(_inputArgs[_index++].get()))
      return node->getFile();
  }
}

void InputGraph::addInputElement(std::unique_ptr<InputElement> ie) {
  _inputArgs.push_back(std::move(ie));
}

void InputGraph::addInputElementFront(std::unique_ptr<InputElement> ie) {
  _inputArgs.insert(_inputArgs.begin(), std::move(ie));
}

// If we are at the end of a group, return its size (which indicates
// how many files we need to go back in the command line).
// Returns 0 if we are not at the end of a group.
int InputGraph::getGroupSize() {
  if (_index >= _inputArgs.size())
    return 0;
  InputElement *elem = _inputArgs[_index].get();
  if (const GroupEnd *group = dyn_cast<GroupEnd>(elem))
    return group->getSize();
  return 0;
}

void InputGraph::skipGroup() {
  if (_index >= _inputArgs.size())
    return;
  if (isa<GroupEnd>(_inputArgs[_index].get()))
    _index++;
}

std::error_code FileNode::parse(const LinkingContext &, raw_ostream &) {
  if (_file)
    if (std::error_code ec = _file->parse())
      return ec;
  return std::error_code();
}
