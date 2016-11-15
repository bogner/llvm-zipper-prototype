//===----------------------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// Check that the overloads of std::__libcpp_{isnan,isinf,isfinite} that take
// floating-point values are evaluatable from constexpr contexts.
//
// These functions need to be constexpr in order to be called from CUDA, see
// https://reviews.llvm.org/D25403.  They don't actually need to be
// constexpr-evaluatable, but that's what we check here, since we can't check
// true constexpr-ness.
//
// UNSUPPORTED: c++98, c++03

#include <cmath>

constexpr bool a = std::__libcpp_isnan(0.);
constexpr bool b = std::__libcpp_isinf(0.0);
constexpr bool c = std::__libcpp_isfinite(0.0);

int main()
{
  return 0;
}
