//===----------------------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// <memory>

// template <class OuterAlloc, class... InnerAllocs>
//   class scoped_allocator_adaptor

// typedef see below propagate_on_container_copy_assignment;

#include <scoped_allocator>
#include <type_traits>

#include "../allocators.h"

int main()
{
#ifdef _LIBCPP_MOVE

    static_assert((std::is_same<
        std::scoped_allocator_adaptor<A1<int>>::propagate_on_container_copy_assignment,
        std::false_type>::value), "");

    static_assert((std::is_same<
        std::scoped_allocator_adaptor<A1<int>, A2<int>>::propagate_on_container_copy_assignment,
        std::false_type>::value), "");

    static_assert((std::is_same<
        std::scoped_allocator_adaptor<A1<int>, A2<int>, A3<int>>::propagate_on_container_copy_assignment,
        std::true_type>::value), "");

#endif  // _LIBCPP_MOVE
}
