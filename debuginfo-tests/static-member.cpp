// RUN: %clangxx -O0 -g %s -o %t -c
// RUN: %clangxx %t -o %t.out
// RUN: %test_debuginfo %s %t.out

// FIXME: The ptype command only works with an LLDB from XCode 5 or
// later and not all buildbots have that yet.
// XFAIL: darwin

// DEBUGGER: delete breakpoints
// DEBUGGER: break static-member.cpp:33
// DEBUGGER: r
// DEBUGGER: ptype MyClass
// CHECK:      {{struct|class}} MyClass {
// CHECK:      static const int a;
// CHECK-NEXT: static int b;
// CHECK-NEXT: static int c;
// CHECK-NEXT: int d;
// CHECK-NEXT: }
// DEBUGGER: p MyClass::a
// CHECK: ${{[0-9]}} = 4
// DEBUGGER: p MyClass::c
// CHECK: ${{[0-9]}} = 15

// PR14471, PR14734

class MyClass {
public:
  const static int a = 4;
  static int b;
  static int c;
  int d;
};

int MyClass::c = 15;
const int MyClass::a;

int main() {
    MyClass instance_MyClass;
    return MyClass::a;
}
