#ifndef LATENCY_CALIBRATION_H
#define LATENCY_CALIBRATION_H
#include <algorithm>
#include <cmath>

struct LatencyOffsets { long inputUs, outputUs; };

inline LatencyOffsets CalibrateLatency(double rate, long bufferSamples, long inputReported,
                                      long outputReported, long measured, LatencyOffsets current) {
    long difference = measured - inputReported - outputReported;
    // A shorter capture duration can reduce the total by more than the output
    // latency. Correct input first when reducing latency, keeping both reported
    // directions nonnegative; any remaining reduction comes from output.
    long inputReduction = difference < 0 ? std::min(inputReported, -difference) : 0;
    current.inputUs -= (long)std::lround(inputReduction * 1000000.0 / rate);
    long outputChange = difference + inputReduction;
    if (outputChange) {
        // Output's baseline is one buffer. Reconstruct its offset from that
        // baseline so an existing offset clamped to zero cannot hide a change.
        current.outputUs = (long)std::lround((outputReported + outputChange - bufferSamples) * 1000000.0 / rate);
    }
    return current;
}
#endif
