//===----------------------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// <locale>

// typedef int category;

#include <locale>
#include <type_traits>
#include <cassert>

int main()
{
    static_assert((std::is_same<std::locale::category, int>::value), "");
    assert(std::locale::none == 0);
    assert(std::locale::collate);
    assert(std::locale::ctype);
    assert(std::locale::monetary);
    assert(std::locale::numeric);
    assert(std::locale::time);
    assert(std::locale::messages);
    assert((std::locale::collate
          & std::locale::ctype
          & std::locale::monetary
          & std::locale::numeric
          & std::locale::time
          & std::locale::messages) == 0);
    assert((std::locale::collate
          | std::locale::ctype
          | std::locale::monetary
          | std::locale::numeric
          | std::locale::time
          | std::locale::messages)
         == std::locale::all);
}
