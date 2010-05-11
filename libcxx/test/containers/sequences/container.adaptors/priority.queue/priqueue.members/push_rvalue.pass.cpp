//===----------------------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// <queue>

// priority_queue();

// void push(value_type&& v);

#include <queue>
#include <cassert>

#include "../../../../MoveOnly.h"

int main()
{
#ifdef _LIBCPP_MOVE
    std::priority_queue<MoveOnly> q;
    q.push(1);
    assert(q.top() == 1);
    q.push(3);
    assert(q.top() == 3);
    q.push(2);
    assert(q.top() == 3);
#endif
}
