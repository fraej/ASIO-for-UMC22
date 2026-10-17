// Shows what ASIO-for-UMC22 reports to hosts such as DAWs, then measures the
// real round-trip latency through a loopback cable and compares the two.
//
// 1. Close other audio programs. Unplug headphones or turn speakers down.
// 2. Connect a UMC22 output to an input, for example the headphone or RCA output
//    to the instrument input, with the input gain low.
// 3. Run one of:
//      latency_test           print the driver's report and measure
//      latency_test --write   also store the correction in asio_settings.ini
//      latency_test --info    only print what the driver reports
#include "../asio_types.h"
#include "latency_calibration.h"
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace {
const double kWarmupSeconds = 1.0;  // Before the first pulse; also measures the noise floor.
const double kSpacingSeconds = 0.5; // Between pulses; longer than any plausible round trip.
const int kPulses = 12;
const int kPulseLength = 48;        // A Hann-windowed 4 kHz burst, -12 dBFS.
const double kPi = 3.14159265358979323846;

struct Session {
    ASIOBufferInfo buffers[4];
    long bufferSize;
    double rate;
    float pulse[kPulseLength];
    std::vector<float> input[2];
    long long position;
    long long start, spacing, total;
    volatile LONG done;
} g;

void BufferSwitch(long index, ASIOBool) {
    float* in[2] = {(float*)g.buffers[0].buffers[index], (float*)g.buffers[1].buffers[index]};
    float* out[2] = {(float*)g.buffers[2].buffers[index], (float*)g.buffers[3].buffers[index]};
    for (long i = 0; i < g.bufferSize; ++i) {
        // Input and output samples of one buffer share a stream position, so the
        // distance between an emitted and a recorded pulse is the round trip.
        long long n = g.position + i;
        float value = 0;
        if (n >= g.start && (n - g.start) / g.spacing < kPulses && (n - g.start) % g.spacing < kPulseLength)
            value = g.pulse[(n - g.start) % g.spacing];
        out[0][i] = out[1][i] = value;
        if (n < g.total) {
            g.input[0][n] = in[0][i];
            g.input[1][n] = in[1][i];
        }
    }
    g.position += g.bufferSize;
    if (g.position >= g.total) InterlockedExchange(&g.done, 1);
}
ASIOTime* BufferSwitchTimeInfo(ASIOTime* time, long index, ASIOBool direct) {
    BufferSwitch(index, direct);
    return time;
}
void SampleRateChanged(ASIOSampleRate) {}
long Message(long selector, long value, void*, double*) {
    if (selector == kAsioSelectorSupported) return value == kAsioEngineVersion;
    if (selector == kAsioEngineVersion) return 2;
    return 0;
}

double Correlate(const std::vector<float>& signal, long long at) {
    double sum = 0;
    for (int j = 0; j < kPulseLength; ++j) sum += signal[at + j] * g.pulse[j];
    return fabs(sum);
}

bool ReadRegistry(const wchar_t* key, const wchar_t* value, wchar_t* out, DWORD chars) {
    DWORD bytes = chars * sizeof(wchar_t);
    return RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ, NULL, out, &bytes) == ERROR_SUCCESS;
}

void PrintReport(IASIO* driver) {
    char name[64] = {};
    driver->getDriverName(name);
    long inputs = 0, outputs = 0, minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
    driver->getChannels(&inputs, &outputs);
    driver->getBufferSize(&minSize, &maxSize, &preferred, &granularity);
    printf("Driver: %s, version %ld\n", name, driver->getDriverVersion());
    for (long direction = 0; direction < 2; ++direction) {
        for (long channel = 0; channel < (direction ? outputs : inputs); ++channel) {
            ASIOChannelInfo info = {};
            info.channel = channel;
            info.isInput = direction ? ASIOFalse : ASIOTrue;
            if (driver->getChannelInfo(&info) == ASE_OK)
                printf("  %s channel %ld: %s (sample type %ld)\n", direction ? "Output" : "Input", channel + 1,
                       info.name, info.type);
        }
    }
    printf("Buffer sizes: %ld to %ld samples in steps of %ld, preferred %ld\n", minSize, maxSize, granularity, preferred);
    printf("Sample rates:");
    for (double rate : {22050.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0})
        if (driver->canSampleRate(rate) == ASE_OK) printf(" %.0f", rate);
    ASIOClockSource clocks[4] = {};
    long clockCount = 4;
    if (driver->getClockSources(clocks, &clockCount) == ASE_OK)
        for (long i = 0; i < clockCount; ++i) printf("\nClock source: %s%s", clocks[i].name, clocks[i].isCurrentSource ? " (current)" : "");
    printf("\nTime info: %s, dropout reports: %s, input monitoring: %s\n",
           driver->future(kAsioCanTimeInfo, NULL) == ASE_SUCCESS ? "yes" : "no",
           driver->future(kAsioCanReportOverload, NULL) == ASE_SUCCESS ? "yes" : "no",
           driver->future(kAsioCanInputMonitor, NULL) == ASE_SUCCESS ? "yes" : "no");
}
}  // namespace

int main(int argc, char** argv) {
    bool write = argc > 1 && strcmp(argv[1], "--write") == 0;
    bool infoOnly = argc > 1 && strcmp(argv[1], "--info") == 0;
    wchar_t clsidText[64], dllPath[MAX_PATH];
    CLSID clsid;
    if (!ReadRegistry(L"SOFTWARE\\ASIO\\ASIO-for-UMC22", L"CLSID", clsidText, 64) || FAILED(CLSIDFromString(clsidText, &clsid))) {
        puts("ASIO-for-UMC22 is not registered. Run regsvr32 on asio_for_umc22.dll first.");
        return 1;
    }
    wchar_t serverKey[128];
    swprintf(serverKey, 128, L"SOFTWARE\\Classes\\CLSID\\%ls\\InprocServer32", clsidText);
    wchar_t settings[MAX_PATH] = L"";
    if (ReadRegistry(serverKey, NULL, dllPath, MAX_PATH)) {
        wchar_t* slash = wcsrchr(dllPath, L'\\');
        if (slash && (slash - dllPath) + 18 < MAX_PATH) {
            wcscpy(slash + 1, L"asio_settings.ini");
            wcscpy(settings, dllPath);
        }
    }

    CoInitialize(NULL);
    IASIO* driver = NULL;
    if (FAILED(CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, clsid, (void**)&driver))) {
        puts("Could not load the registered ASIO-for-UMC22 DLL.");
        return 1;
    }
    char error[128] = {};
    if (!driver->init(GetConsoleWindow())) {
        driver->getErrorMessage(error);
        printf("The driver did not initialize: %s\n", error);
        return 1;
    }
    PrintReport(driver);

    long minSize, maxSize, granularity;
    driver->getBufferSize(&minSize, &maxSize, &g.bufferSize, &granularity);
    driver->getSampleRate(&g.rate);
    for (int i = 0; i < 4; ++i) {
        g.buffers[i].isInput = i < 2 ? ASIOTrue : ASIOFalse;
        g.buffers[i].channelNum = i % 2;
    }
    ASIOCallbacks callbacks = {BufferSwitch, SampleRateChanged, Message, BufferSwitchTimeInfo};
    if (driver->createBuffers(g.buffers, 4, g.bufferSize, &callbacks) != ASE_OK) {
        driver->getErrorMessage(error);
        printf("The driver could not open the device: %s\n", error);
        return 1;
    }
    long inputLatency = 0, outputLatency = 0;
    driver->getLatencies(&inputLatency, &outputLatency);
    ASIOInternalBufferInfo internal = {};
    bool hasInternal = driver->future(kAsioGetInternalBufferSamples, &internal) == ASE_SUCCESS;
    double msPerSample = 1000.0 / g.rate;
    printf("Opened at %.0f Hz with %ld-sample buffers.\n", g.rate, g.bufferSize);
    printf("Latency reported to hosts: input %ld samples (%.2f ms), output %ld samples (%.2f ms), "
           "round trip %.2f ms\n", inputLatency, inputLatency * msPerSample, outputLatency, outputLatency * msPerSample,
           (inputLatency + outputLatency) * msPerSample);
    if (hasInternal)
        printf("  of which driver-internal buffering: input %ld samples, output %ld samples\n",
               internal.inputSamples, internal.outputSamples);
    if (infoOnly) {
        driver->disposeBuffers();
        driver->Release();
        return 0;
    }

    for (int j = 0; j < kPulseLength; ++j) {
        double window = 0.5 - 0.5 * cos(2 * kPi * (j + 0.5) / kPulseLength);
        g.pulse[j] = (float)(0.25 * window * sin(2 * kPi * 4000.0 * j / g.rate));
    }
    g.start = (long long)(kWarmupSeconds * g.rate);
    g.spacing = (long long)(kSpacingSeconds * g.rate);
    g.total = g.start + kPulses * g.spacing + g.spacing;
    g.input[0].assign(g.total, 0.0f);
    g.input[1].assign(g.total, 0.0f);
    printf("\nMeasuring with %d pulses on both outputs...\n", kPulses);
    if (driver->start() != ASE_OK) {
        driver->getErrorMessage(error);
        printf("The driver could not start: %s\n", error);
        return 1;
    }
    for (int i = 0; i < 400 && !g.done; ++i) Sleep(50);
    driver->stop();
    driver->disposeBuffers();
    driver->Release();
    if (!g.done) {
        puts("The driver stopped delivering buffers before the measurement finished.");
        return 1;
    }

    // Detect each pulse by correlation, above both the noise floor seen during the
    // warm-up and a 1% loop gain. Use the input channel that caught the most pulses.
    double energy = 0;
    for (float sample : g.pulse) energy += (double)sample * sample;
    long window = (long)(kSpacingSeconds * g.rate) - kPulseLength;
    std::vector<long> delays[2];
    for (int channel = 0; channel < 2; ++channel) {
        double noise = 0;
        for (long long at = (long long)(0.1 * g.rate); at + kPulseLength < g.start; ++at)
            noise = std::max(noise, Correlate(g.input[channel], at));
        double threshold = std::max(4 * noise, 0.01 * energy);
        for (int k = 0; k < kPulses; ++k) {
            long long emitted = g.start + k * g.spacing;
            double best = 0;
            long bestDelay = -1;
            for (long d = 0; d < window; ++d) {
                double c = Correlate(g.input[channel], emitted + d);
                if (c > best) { best = c; bestDelay = d; }
            }
            if (best >= threshold) delays[channel].push_back(bestDelay);
        }
    }
    int channel = delays[1].size() > delays[0].size() ? 1 : 0;
    std::vector<long>& found = delays[channel];
    if (found.size() < (size_t)kPulses / 2) {
        puts("No loopback signal detected. Connect an output to an input, raise the input gain a little, and retry.");
        return 2;
    }
    std::sort(found.begin(), found.end());
    long roundTrip = found[found.size() / 2];
    long reported = inputLatency + outputLatency;
    long difference = roundTrip - reported;
    printf("Measured round trip on In %d (%u of %d pulses): %ld samples (%.2f ms), spread %ld samples\n",
           channel + 1, (unsigned)found.size(), kPulses, roundTrip, roundTrip * msPerSample, found.back() - found.front());
    printf("The driver %s the round trip by %ld samples (%.2f ms).\n",
           difference >= 0 ? "under-reports" : "over-reports", labs(difference), labs(difference) * msPerSample);
    if (!settings[0]) return 0;

    wchar_t text[32] = L"";
    GetPrivateProfileStringW(L"Audio", L"InputLatencyOffset", L"0", text, 32, settings);
    long currentInput = wcstol(text, NULL, 10);
    GetPrivateProfileStringW(L"Audio", L"OutputLatencyOffset", L"0", text, 32, settings);
    long currentOutput = wcstol(text, NULL, 10);
    LatencyOffsets recommended = CalibrateLatency(g.rate, g.bufferSize, inputLatency, outputLatency,
                                                  roundTrip, {currentInput, currentOutput});
    printf("Recommended in %ls:\n  InputLatencyOffset=%ld (currently %ld)\n"
           "  OutputLatencyOffset=%ld (currently %ld)\n", settings, recommended.inputUs, currentInput,
           recommended.outputUs, currentOutput);
    if (write) {
        swprintf(text, 32, L"%ld", recommended.inputUs);
        BOOL savedInput = WritePrivateProfileStringW(L"Audio", L"InputLatencyOffset", text, settings);
        DWORD error = savedInput ? ERROR_SUCCESS : GetLastError();
        swprintf(text, 32, L"%ld", recommended.outputUs);
        BOOL savedOutput = WritePrivateProfileStringW(L"Audio", L"OutputLatencyOffset", text, settings);
        if (!savedOutput) error = GetLastError();
        if (savedInput && savedOutput)
            puts("Saved. Reopen the driver in your host to apply it.");
        else
            printf("Could not save both offsets (Windows error %lu). Check both settings before reopening the driver.\n", error);
    }
    return 0;
}
