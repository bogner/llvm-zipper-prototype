//===---------------------------- system_error ------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This is a temporary file to help with the transition to std::error_code.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_SYSTEM_ERROR_H
#define LLVM_SUPPORT_SYSTEM_ERROR_H

#include <system_error>

namespace llvm {
using std::error_code;
using std::is_error_condition_enum;
using std::is_error_code_enum;
using std::system_category;
using std::generic_category;
using std::error_category;
using std::make_error_code;
using std::error_condition;
}

#endif
