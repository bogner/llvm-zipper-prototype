//===----------------------------------------------------------------------===//
//
// ��������������������The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// <future>

// enum class future_errc
// {
//     broken_promise,
//     future_already_retrieved,
//     promise_already_satisfied,
//     no_state
// };

#include <future>

int main()
{
    static_assert(std::future_errc::broken_promise == 0, "");
    static_assert(std::future_errc::future_already_retrieved == 1, "");
    static_assert(std::future_errc::promise_already_satisfied == 2, "");
    static_assert(std::future_errc::no_state == 3, "");
}
