//===----------------------------------------------------------------------===//
//
// ��������������������The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// <iterator>

// reverse_iterator

// explicit reverse_iterator(Iter x);

#include <iterator>
#include <cassert>

#include "../../../../iterators.h"

template <class It>
void
test(It i)
{
    std::reverse_iterator<It> r(i);
    assert(r.base() == i);
}

int main()
{
    const char s[] = "123";
    test(bidirectional_iterator<const char*>(s));
    test(random_access_iterator<const char*>(s));
    test(s);
}
