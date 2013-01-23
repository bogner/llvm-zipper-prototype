//===- lld/Driver/Target.h - Linker Target Abstraction --------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// Interface and factory for creating a specific Target. A Target is used to
/// encapsulate all of the target specific configurations for the linker.
///
//===----------------------------------------------------------------------===//

#ifndef LLD_DRIVER_TARGET_H
#define LLD_DRIVER_TARGET_H

#include "lld/Core/TargetInfo.h"

#include "llvm/Support/ErrorOr.h"

#include <memory>

namespace lld {
class LinkerInput;
struct LinkerOptions;
class Reader;
class TargetInfo;
class Writer;

/// \brief Represents a specific target.
class Target {
protected:
  Target(std::unique_ptr<TargetInfo> ti) : _targetInfo(std::move(ti)) {}

public:
  virtual ~Target();

  const TargetInfo &getTargetInfo() const { return *_targetInfo; };

  /// \brief Get a reference to a Reader for the given input.
  ///
  /// Will always return the same object for the same input.
  virtual ErrorOr<lld::Reader&> getReader(const LinkerInput &input) = 0;

  /// \brief Get the writer.
  virtual ErrorOr<lld::Writer&> getWriter() = 0;

  static std::unique_ptr<Target> create(const LinkerOptions&);

protected:
  std::unique_ptr<TargetInfo> _targetInfo;
};
}

#endif
