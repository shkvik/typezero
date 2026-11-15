#include <stdint.h>

int32_t count_bytes(const uint8_t *data, int32_t length, uint8_t wanted) {
  int32_t found = 0;
  for (int32_t i = 0; i < length; ++i) {
    if (data[i] == wanted) {
      found += 1;
    }
  }
  return found;
}
