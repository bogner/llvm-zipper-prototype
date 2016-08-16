//===-- Stream.cpp - General stream implementation ------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the implementation details for a general stream object.
///
//===----------------------------------------------------------------------===//

#include "streamexecutor/Stream.h"

namespace streamexecutor {

Stream::Stream(std::unique_ptr<PlatformStreamHandle> PStream)
    : PlatformExecutor(PStream->getExecutor()),
      ThePlatformStream(std::move(PStream)) {}

Stream::~Stream() = default;

} // namespace streamexecutor
