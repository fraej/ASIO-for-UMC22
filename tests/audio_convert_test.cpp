#include "../audio_convert.h"
#include <assert.h>
#include <limits>
#include <stdio.h>
#include <string.h>
#include <random>
#include <vector>

int main() {
    // Every signed PCM value must survive a float round-trip, including -32768.
    const int frames = 65536;
    std::vector<int16_t> input(frames * 2), output(frames * 2);
    std::vector<float> left(frames), right(frames);
    for (int i = 0; i < frames; ++i) {
        input[i * 2] = static_cast<int16_t>(i - 32768);
        input[i * 2 + 1] = static_cast<int16_t>(32767 - i);
    }
    PcmToFloat(input.data(), left.data(), right.data(), frames);
    FloatToPcm(left.data(), right.data(), output.data(), frames);
    assert(input == output);
    const float values[] = {0, -1, 1, -2, 2, 0.5f, -0.5f,
        std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), 0.5f / 32768, 1.5f / 32768};
    const int16_t expected[] = {0, -32768, 32767, -32768, 32767, 16384, -16384, 0, 32767, -32768, 0, 2};
    _MM_SET_ROUNDING_MODE(_MM_ROUND_NEAREST);
    for (int n = 1; n <= 12; ++n) {
        int16_t pcm[28];
        for (int16_t& value : pcm) value = 12345;
        FloatToPcm(values, NULL, pcm + 1, n); // Deliberately unaligned; includes scalar tails.
        assert(pcm[0] == 12345 && pcm[n * 2 + 1] == 12345);
        for (int i = 0; i < n; ++i) {
            assert(pcm[i * 2 + 1] == expected[i]);
            assert(pcm[i * 2 + 2] == 0);
        }
        float l[12], r[12];
        PcmToFloat(pcm + 1, l, r, n);
        for (int i = 0; i < n; ++i) {
            assert(l[i] == expected[i] / 32768.0f);
            assert(r[i] == 0);
        }
    }
    // Exercise the eight-frame stereo path, every supported driver length and
    // tails against scalar quantization, with distinct channels and guard data.
    const unsigned savedMxcsr = _mm_getcsr();
    std::mt19937 random(42);
    std::vector<float> l(1010), r(1010);
    for (int i = 0; i < 1010; ++i) {
        uint32_t a = random(), b = random();
        memcpy(&l[i], &a, sizeof(a));
        memcpy(&r[i], &b, sizeof(b));
    }
    for (int i = 0; i < 12; ++i) { l[i+1] = values[i]; r[12-i] = values[i]; }
    std::vector<int> sizes;
    for (int n = 0; n <= 33; ++n) sizes.push_back(n);
    for (int n = 48; n <= 1008; n += 48) sizes.push_back(n);
    for (unsigned mode : {_MM_ROUND_NEAREST, _MM_ROUND_DOWN, _MM_ROUND_UP, _MM_ROUND_TOWARD_ZERO}) {
        for (unsigned denormal : {0u, 0x40u, 0x8000u, 0x8040u}) {
            _mm_setcsr(0x1f80u | mode | denormal);
            for (int n : sizes) {
                for (int channels = 0; channels < 4; ++channels) {
                    // Allocate exactly enough for the output plus two guards;
                    // ASan also catches writes beyond that allocation.
                    std::vector<int16_t> actual(n*2+2, 12345), expected(actual);
                    const float* lp = channels & 1 ? l.data()+1 : NULL;
                    const float* rp = channels & 2 ? r.data()+1 : NULL;
                    for (int i = 0; i < n; ++i) {
                        expected[1+2*i] = QuantizeSample(lp ? lp[i] : 0.0f);
                        expected[2+2*i] = QuantizeSample(rp ? rp[i] : 0.0f);
                    }
                    FloatToPcm(lp, rp, actual.data()+1, n);
                    assert(actual == expected);
                }
            }
        }
    }
    _mm_setcsr(savedMxcsr);
    puts("PASS audio conversion: exhaustive PCM, random floats, clipping, NaN/infinity, rounding/DAZ/FTZ, tails, channels and bounds");
}
