#include "multiple_class_test.h"

namespace a {
int Move1::f() {
  return 0;
}
} // namespace a

namespace b {
int Move2::f() {
  return 0;
}
} // namespace b

namespace c {
int Move3::f() {
  return 0;
}

int Move4::f() {
  return 0;
}

int NoMove::f() {
  return 0;
}
} // namespace c
