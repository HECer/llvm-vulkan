// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Small semantic smoke input, not evidence for end-to-end compile-time speedup.
#include <stdint.h>
#include <stdio.h>

static uint32_t checksum(const uint32_t *data, unsigned n) {
  uint32_t value = 2166136261u;
  for (unsigned i = 0; i < n; ++i) value = (value ^ data[i]) * 16777619u;
  return value;
}

int main(void) {
  const uint32_t data[] = {1, 3, 7, 15, 31, 63, 127, 255};
  printf("%u\n", checksum(data, sizeof(data) / sizeof(data[0])));
  return 0;
}
