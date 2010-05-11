//===----------------------------------------------------------------------===//
//
// ��������������������The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// <random>

// template<class Engine, size_t p, size_t r>
// class discard_block_engine
// {
// public:
//     // types
//     typedef typename Engine::result_type result_type;
// 
//     // engine characteristics
//     static constexpr size_t block_size = p;
//     static constexpr size_t used_block = r;
//     static constexpr result_type min() { return Engine::min(); }
//     static constexpr result_type max() { return Engine::max(); }

#include <random>
#include <type_traits>
#include <cassert>

void
test1()
{
    typedef std::ranlux24 E;
    static_assert((E::block_size == 223), "");
    static_assert((E::used_block == 23), "");
    /*static_*/assert((E::min() == 0)/*, ""*/);
    /*static_*/assert((E::max() == 0xFFFFFF)/*, ""*/);
}

void
test2()
{
    typedef std::ranlux48 E;
    static_assert((E::block_size == 389), "");
    static_assert((E::used_block == 11), "");
    /*static_*/assert((E::min() == 0)/*, ""*/);
    /*static_*/assert((E::max() == 0xFFFFFFFFFFFFull)/*, ""*/);
}

int main()
{
    test1();
    test2();
}
