//===----------------------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// <strstream>

// class ostrstream

// ostrstream();

#include <strstream>
#include <cassert>

int main()
{
    std::ostrstream out;
    int i = 123;
    double d = 4.5;
    std::string s("dog");
    out << i << ' ' << d << ' ' << s;
    assert(out.str() == std::string("123 4.5 dog"));
}
