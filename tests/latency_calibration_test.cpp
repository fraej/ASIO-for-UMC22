#include "../tools/latency_calibration.h"
#include <assert.h>
#include <stdio.h>

static long Report(long baseline, long offset, double rate) {
    double result = baseline + offset * rate / 1000000.0;
    return result > 0 ? (long)(result + 0.5) : 0;
}

int main() {
    // A 7 ms capture reduction used to produce OutputLatencyOffset=-7000,
    // clamping the 2 ms output report to zero and leaving 5 ms uncorrected.
    LatencyOffsets result = CalibrateLatency(48000, 96, 576, 96, 336, {0, 0});
    assert(result.inputUs == -7000 && result.outputUs == 0);
    for (double rate : {44100.0, 48000.0}) {
        for (long buffer : {48L, 96L, 192L, 1008L}) {
            long inputBase = buffer + (long)rate / 100;
            for (long input : {-20000L, -3000L, 0L, 3000L, 100000L}) {
                for (long output : {-20000L, -1000L, 0L, 1000L, 100000L}) {
                    long in = Report(inputBase, input, rate), out = Report(buffer, output, rate);
                    for (long measured : {0L, 1L, 144L, 336L, 672L, 4096L}) {
                        LatencyOffsets next = CalibrateLatency(rate, buffer, in, out, measured, {input, output});
                        long corrected = Report(inputBase, next.inputUs, rate) + Report(buffer, next.outputUs, rate);
                        assert(std::abs(corrected - measured) <= 1);
                    }
                }
            }
        }
    }
    puts("PASS latency calibration: shorter capture, positive/negative corrections, clamped offsets and both rates");
}
