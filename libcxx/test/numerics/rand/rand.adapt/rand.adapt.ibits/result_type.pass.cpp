//===----------------------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// <random>

// template<class Engine, size_t w, class UIntType>
// class independent_bits_engine
// {
// public:
//     // types
//     typedef UIntType result_type;

#include <random>
#include <type_traits>

template <class UIntType, UIntType Min, UIntType Max>
class rand1
{
public:
    // types
    typedef UIntType result_type;

private:
    result_type x_;

    static_assert(Min < Max, "rand1 invalid parameters");
public:

    // Temporary work around for lack of constexpr
    static const result_type _Min = Min;
    static const result_type _Max = Max;

    static const/*expr*/ result_type min() {return Min;}
    static const/*expr*/ result_type max() {return Max;}

    explicit rand1(result_type sd = Min) : x_(sd)
    {
        if (x_ < Min)
            x_ = Min;
        if (x_ > Max)
            x_ = Max;
    }

    result_type operator()()
    {
        result_type r = x_;
        if (x_ < Max)
            ++x_;
        else
            x_ = Min;
        return r;
    }
};

void
test1()
{
    static_assert((std::is_same<
        std::independent_bits_engine<rand1<unsigned long, 0, 10>, 16, unsigned>::result_type,
        unsigned>::value), "");
}

void
test2()
{
    static_assert((std::is_same<
        std::independent_bits_engine<rand1<unsigned long, 0, 10>, 16, unsigned long long>::result_type,
        unsigned long long>::value), "");
}

int main()
{
    test1();
    test2();
}
