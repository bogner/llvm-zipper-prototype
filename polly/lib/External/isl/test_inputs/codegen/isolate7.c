{
  for (int c0 = 0; c0 < n - 31; c0 += 32)
    for (int c1 = 0; c1 <= n; c1 += 32) {
      if (n >= c1 + 32) {
        for (int c2 = 0; c2 <= 31; c2 += 1)
          for (int c3 = 0; c3 <= 31; c3 += 1)
            S_1(c0 + c2, c1 + c3);
      } else
        for (int c2 = 0; c2 <= 31; c2 += 1) {
          for (int c3 = 0; c3 < n - c1; c3 += 1)
            S_1(c0 + c2, c1 + c3);
          S_2(c0 + c2);
        }
    }
  if (n >= 32) {
    for (int c1 = 0; c1 < n; c1 += 32) {
      if (n >= c1 + 32) {
        for (int c2 = 0; c2 < n % 32; c2 += 1)
          for (int c3 = 0; c3 <= 31; c3 += 1)
            S_1(-((n - 1) % 32) + n + c2 - 1, c1 + c3);
      } else
        for (int c2 = 0; c2 < n - c1; c2 += 1) {
          for (int c3 = 0; c3 < n - c1; c3 += 1)
            S_1(c1 + c2, c1 + c3);
          S_2(c1 + c2);
        }
    }
  } else
    for (int c2 = 0; c2 < n; c2 += 1) {
      for (int c3 = 0; c3 < n; c3 += 1)
        S_1(c2, c3);
      S_2(c2);
    }
}
