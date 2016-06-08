//===- StreamRef.h - A copyable reference to a stream -----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DEBUGINFO_CODEVIEW_STREAMREF_H
#define LLVM_DEBUGINFO_CODEVIEW_STREAMREF_H

#include "llvm/DebugInfo/CodeView/CodeViewError.h"
#include "llvm/DebugInfo/CodeView/StreamInterface.h"

namespace llvm {
namespace codeview {

class StreamRef : private StreamInterface {
public:
  StreamRef() : Stream(nullptr), ViewOffset(0), Length(0) {}
  StreamRef(const StreamInterface &Stream)
      : Stream(&Stream), ViewOffset(0), Length(Stream.getLength()) {}
  StreamRef(const StreamInterface &Stream, uint32_t Offset, uint32_t Length)
      : Stream(&Stream), ViewOffset(Offset), Length(Length) {}

  StreamRef(const StreamRef &Stream, uint32_t Offset, uint32_t Length) = delete;

  Error readBytes(uint32_t Offset, uint32_t Size,
                  ArrayRef<uint8_t> &Buffer) const override {
    if (Size + Offset > Length)
      return make_error<CodeViewError>(cv_error_code::insufficient_buffer);
    return Stream->readBytes(ViewOffset + Offset, Size, Buffer);
  }

  uint32_t getLength() const override { return Length; }
  StreamRef drop_front(uint32_t N) const {
    if (!Stream)
      return StreamRef();

    N = std::min(N, Length);
    return StreamRef(*Stream, ViewOffset + N, Length - N);
  }

  StreamRef keep_front(uint32_t N) const {
    if (!Stream)
      return StreamRef();
    N = std::min(N, Length);
    return StreamRef(*Stream, ViewOffset, N);
  }

  StreamRef slice(uint32_t Offset, uint32_t Len) const {
    return drop_front(Offset).keep_front(Len);
  }

  bool operator==(const StreamRef &Other) const {
    if (Stream != Other.Stream)
      return false;
    if (ViewOffset != Other.ViewOffset)
      return false;
    if (Length != Other.Length)
      return false;
    return true;
  }

  bool operator!=(const StreamRef &Other) const { return !(*this == Other); }

private:
  const StreamInterface *Stream;
  uint32_t ViewOffset;
  uint32_t Length;
};
}
}

#endif // LLVM_DEBUGINFO_CODEVIEW_STREAMREF_H
