//===----------------------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// <chrono>

// duration

// duration() = default;

// Rep must be default initialized, not initialized with 0

#include <chrono>
#include <cassert>

#include "../../rep.h"

template <class D>
void
test()
{
    D d;
    assert(d.count() == typename D::rep());
}

int main()
{
    test<std::chrono::duration<Rep> >();
}
