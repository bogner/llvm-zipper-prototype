// RUN: cat %s > %t.cpp
// RUN: clang-rename -offset=158 -new-name=Y %t.cpp -i --
// RUN: sed 's,//.*,,' %t.cpp | FileCheck %s
class C
{
public:
  static int X;
};

int foo(int x)
{
  return 0;
}
#define FOO(a) foo(a)

int main()
{
  C::X = 1; // CHECK: C::Y
  FOO(C::X); // CHECK: C::Y
  int y = C::X; // CHECK: C::Y
}

// Use grep -FUbo 'X' <file> to get the correct offset of foo when changing
// this file.
