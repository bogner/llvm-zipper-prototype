// RUN: %check_clang_tidy %s abseil-duration-factory-scale %t

// Mimic the implementation of absl::Duration
namespace absl {

class Duration {};

Duration Nanoseconds(long long);
Duration Microseconds(long long);
Duration Milliseconds(long long);
Duration Seconds(long long);
Duration Minutes(long long);
Duration Hours(long long);

#define GENERATE_DURATION_FACTORY_OVERLOADS(NAME) \
  Duration NAME(float n);                         \
  Duration NAME(double n);                        \
  template <typename T>                           \
  Duration NAME(T n);

GENERATE_DURATION_FACTORY_OVERLOADS(Nanoseconds);
GENERATE_DURATION_FACTORY_OVERLOADS(Microseconds);
GENERATE_DURATION_FACTORY_OVERLOADS(Milliseconds);
GENERATE_DURATION_FACTORY_OVERLOADS(Seconds);
GENERATE_DURATION_FACTORY_OVERLOADS(Minutes);
GENERATE_DURATION_FACTORY_OVERLOADS(Hours);
#undef GENERATE_DURATION_FACTORY_OVERLOADS

}  // namespace absl

void ScaleTest() {
  absl::Duration d;

  // Zeroes
  d = absl::Hours(0);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: use ZeroDuration() for zero-length time intervals [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::ZeroDuration();
  d = absl::Minutes(0);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: use ZeroDuration() for zero-length time intervals [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::ZeroDuration();
  d = absl::Seconds(0);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: use ZeroDuration() for zero-length time intervals [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::ZeroDuration();
  d = absl::Milliseconds(0);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: use ZeroDuration() for zero-length time intervals [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::ZeroDuration();
  d = absl::Microseconds(0);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: use ZeroDuration() for zero-length time intervals [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::ZeroDuration();
  d = absl::Nanoseconds(0);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: use ZeroDuration() for zero-length time intervals [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::ZeroDuration();
  d = absl::Seconds(0.0);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: use ZeroDuration() for zero-length time intervals [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::ZeroDuration();
  d = absl::Seconds(0x0.000001p-126f);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: use ZeroDuration() for zero-length time intervals [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::ZeroDuration();

  // Fold seconds into minutes
  d = absl::Seconds(30 * 60);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: internal duration scaling can be removed [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::Minutes(30);
  d = absl::Seconds(60 * 30);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: internal duration scaling can be removed [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::Minutes(30);

  // Try a few more exotic multiplications
  d = absl::Seconds(60 * 30 * 60);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: internal duration scaling can be removed [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::Minutes(60 * 30);
  d = absl::Seconds(1e-3 * 30);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: internal duration scaling can be removed [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::Milliseconds(30);
  d = absl::Milliseconds(30 * 1000);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: internal duration scaling can be removed [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::Seconds(30);
  d = absl::Milliseconds(30 * 0.001);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: internal duration scaling can be removed [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::Microseconds(30);

  // Multiple steps
  d = absl::Seconds(5 * 3600);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: internal duration scaling can be removed [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::Hours(5);
  d = absl::Microseconds(5 * 1e6);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: internal duration scaling can be removed [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::Seconds(5);
  d = absl::Seconds(5 * 1e-6);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: internal duration scaling can be removed [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::Microseconds(5);
  d = absl::Microseconds(5 * 1000000);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: internal duration scaling can be removed [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::Seconds(5);

  // Division
  d = absl::Hours(30 / 60.);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: internal duration scaling can be removed [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::Minutes(30);
  d = absl::Seconds(30 / 1000.);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: internal duration scaling can be removed [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::Milliseconds(30);
  d = absl::Milliseconds(30 / 1e3);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: internal duration scaling can be removed [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::Microseconds(30);
  d = absl::Seconds(30 / 1e6);
  // CHECK-MESSAGES: :[[@LINE-1]]:7: warning: internal duration scaling can be removed [abseil-duration-factory-scale]
  // CHECK-FIXES: absl::Microseconds(30);

  // None of these should trigger the check
  d = absl::Seconds(60);
  d = absl::Seconds(60 + 30);
  d = absl::Seconds(60 - 30);
  d = absl::Seconds(50 * 30);
  d = absl::Hours(60 * 60);
  d = absl::Nanoseconds(1e-3 * 30);
  d = absl::Seconds(1000 / 30);
  // We don't support division by integers, which could cause rounding
  d = absl::Seconds(10 / 1000);
  d = absl::Seconds(30 / 50);

#define EXPRESSION 30 * 60
  d = absl::Seconds(EXPRESSION);
#undef EXPRESSION

// This should not trigger
#define HOURS(x) absl::Minutes(60 * x)
  d = HOURS(40);
#undef HOURS
}
