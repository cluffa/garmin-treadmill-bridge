#include <assert.h>
#include <stdio.h>
#include "rsc_encode.h"

int main(void) {
    treadmill_state_t s = {0};
    s.speed_mps = 8.0f * 1000.0f / 3600.0f; // 2.2222 m/s
    s.distance_m = 1234.5f;
    uint8_t out[8];
    size_t n = rsc_encode_measurement(&s, out, sizeof out);
    assert(n == 8);
    assert(out[0] == 0x06);              // flags: total distance + running
    // speed = round(2.2222 * 256) = 569 = 0x0239
    assert(out[1] == 0x39 && out[2] == 0x02);
    assert(out[3] == 0xFF);              // cadence: invalid (0xFF)= 0
    // distance = round(1234.5 * 10) = 12345 = 0x00003039
    assert(out[4] == 0x39 && out[5] == 0x30 && out[6] == 0x00 && out[7] == 0x00);

    // Test error diffusion over 1000 frames
    // 2.222222... * 256 = 568.8888...
    // Expected sum after 1000 frames: 1000 * (8/3.6 * 256) = 568888.888...
    uint32_t total_speed_256 = 569; // From the first call above
    for (int i = 1; i < 1000; i++) {
        n = rsc_encode_measurement(&s, out, sizeof out);
        total_speed_256 += out[1] | (out[2] << 8);
    }
    // 568889 is the rounded value of 568888.888...
    assert(total_speed_256 == 568889);

    printf("rsc_encode: OK\n");
    return 0;
}
