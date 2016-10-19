#include "multiple_class_test.h"

namespace a {
int Move1::f() {
  return 0;
}
} // namespace a

namespace {
using a::Move1;
using namespace a;
static int k = 0;
} // anonymous namespace

namespace b {
using a::Move1;
using namespace a;
using T = a::Move1;
int Move2::f() {
  return 0;
}
} // namespace b

namespace c {
int Move3::f() {
  using a::Move1;
  using namespace b;
  return 0;
}

int Move4::f() {
  return 0;
}

int EnclosingMove5::a = 1;

int EnclosingMove5::Nested::f() {
  return 0;
}

int EnclosingMove5::Nested::b = 1;

int NoMove::f() {
  static int F = 0;
  return 0;
}
} // namespace c
