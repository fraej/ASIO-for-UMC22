#ifndef KS_ENGINE_H
#define KS_ENGINE_H
#include <windows.h>
#include <ks.h>
#include <ksmedia.h>
#ifdef __cplusplus
extern "C" {
#endif
// Per direction: one packet with the host, at least one queued.
#define KS_MIN_PACKETS 2
// Optional output safety: each extra queued output buffer adds one buffer of latency.
#define KS_MAX_EXTRA_OUTPUT_BUFFERS 2
#define KS_MAX_RENDER_PACKETS (KS_MIN_PACKETS + KS_MAX_EXTRA_OUTPUT_BUFFERS)
// Enough capture requests to see 8 ms of queued input at 1 ms buffers.
#define KS_MAX_CAPTURE_PACKETS 9
#define KS_MIN_BUFFER_SAMPLES 48
#define KS_MAX_BUFFER_SAMPLES 1008
#define KS_BUFFER_GRANULARITY 48
#define KS_DEVICE_NAME_CHARS 256
#define KS_MAX_RECOVERY_SKIPS 4

// Application-defined error codes (bit 29) for failures that Windows reports
// with misleading generic text, such as "insufficient system resources".
#define KS_ERROR_DEVICE_IN_USE      0x20000001UL
#define KS_ERROR_DEVICE_NOT_FOUND   0x20000002UL
#define KS_ERROR_FORMAT_UNSUPPORTED 0x20000003UL

typedef struct {
    HANDLE event;
    OVERLAPPED overlapped;
    KSSTREAM_HEADER header;
    BYTE* data;
    BOOL pending;
    BOOL locked;
} KSPacket;

typedef struct {
    LONGLONG callbacks;
    LONGLONG waitTimeouts;
    LONGLONG ioErrors;
    LONGLONG shortReads;
    LONGLONG callbackOverruns;
    LONGLONG maxCallbackTicks;
    LONGLONG maxCallbackGapTicks;
    LONGLONG counterFrequency;
    LONGLONG dropouts;    // Output ran out of queued audio.
    LONGLONG inputSkips;  // Input buffers skipped to realign input with output.
    LONGLONG slipSamples; // Total time output was delayed by dropouts.
    LONGLONG inputTrims;  // Queued input buffers trimmed by input alignment.
    LONG queuedInputSamples; // Input queued ahead at its minimum in the last window; -1 before one.
    LONG inputReserve;    // Input buffers alignment keeps queued.
    LONG pageLockFailures;
    DWORD lastError;
} KSStats;

typedef struct {
    HANDLE hFilter;
    HANDLE hRenderPin;
    HANDLE hCapturePin;
    HMODULE hKsUser;
    KSPacket render[KS_MAX_RENDER_PACKETS];
    KSPacket capture[KS_MAX_CAPTURE_PACKETS];
    ULONG renderPackets;  // KS_MIN_PACKETS plus extra output buffers.
    ULONG capturePackets; // Chosen from the buffer size by Init.
    ULONG sampleRate;
    ULONG channels;
    ULONG bitsPerSample;
    ULONG bufferSizeSamples;
    ULONG bufferSizeBytes;
    HANDLE hStopEvent;
    HANDLE hReadyEvent;
    HANDLE hLaunchEvent;
    HANDLE hThread;
    DWORD threadId;
    volatile LONG isRunning;
    // Accessed through atomic builtins; the event still wakes blocking waits.
    LONG stopRequested;
    volatile LONG lastError;
    volatile LONG pageLockFailures;
    volatile LONG queuedInputSamples;
    volatile LONG inputReserve;
    // Interlocked 64-bit counters require eight-byte alignment on Windows.
    DECLSPEC_ALIGN(8) volatile LONG64 callbacks;
    DECLSPEC_ALIGN(8) volatile LONG64 waitTimeouts;
    DECLSPEC_ALIGN(8) volatile LONG64 ioErrors;
    DECLSPEC_ALIGN(8) volatile LONG64 shortReads;
    DECLSPEC_ALIGN(8) volatile LONG64 callbackOverruns;
    DECLSPEC_ALIGN(8) volatile LONG64 maxCallbackTicks;
    DECLSPEC_ALIGN(8) volatile LONG64 maxCallbackGapTicks;
    DECLSPEC_ALIGN(8) volatile LONG64 dropouts;
    DECLSPEC_ALIGN(8) volatile LONG64 inputSkips;
    DECLSPEC_ALIGN(8) volatile LONG64 slipSamples;
    DECLSPEC_ALIGN(8) volatile LONG64 inputTrims;
    LARGE_INTEGER counterFrequency;
    void* callbackContext;
    // Copy/convert input before returning: its packet is immediately resubmitted.
    void (*onInputReady)(void*, long, const void*);
    void (*onBufferReady)(void*, long, void*, ULONGLONG);
    void (*onError)(void*, DWORD);
    ULONG inputDelaySamples; // Input held back by the device driver beyond our packets.
    ULONG captureBufferMs;   // Requested duration; acceptance is not a latency measurement.
    DWORD captureBufferError;
    WCHAR deviceName[KS_DEVICE_NAME_CHARS]; // Name of the opened device, for diagnostics.
    WCHAR deviceService[32]; // Windows driver bound to the device, e.g. "usbaudio".
} KSEngine;

// deviceName selects a device by case-insensitive substring of its name, instance ID,
// or interface path. NULL or empty selects the first VID_08BB&PID_2902 device.
BOOL KSEngine_Init(KSEngine*, ULONG sampleRate, ULONG channels, ULONG bitsPerSample, ULONG bufferSizeSamples,
                   const WCHAR* deviceName);
// Optional SET-only USB capture hint: 0 leaves the pin unchanged, otherwise 3..10 ms.
// Call while stopped, after Init. Failure leaves normal streaming available.
BOOL KSEngine_SetCaptureBufferDuration(KSEngine*, ULONG milliseconds);
// Optional output safety: 0..KS_MAX_EXTRA_OUTPUT_BUFFERS more queued output buffers.
// Each lets the host be one buffer later without a dropout, and adds one buffer of
// output latency. Call while stopped, after Init.
BOOL KSEngine_SetExtraOutputBuffers(KSEngine*, ULONG extra);
BOOL KSEngine_Start(KSEngine*, void (*onInputReady)(void*, long, const void*),
                   void (*onBufferReady)(void*, long, void*, ULONGLONG),
                   void (*onError)(void*, DWORD), void* context);
// Nonblocking; safe from the audio callback. Stop joins on the control thread.
void KSEngine_RequestStop(KSEngine*);
// Lifecycle calls must be serialized by the caller. A callback may request stop;
// it must return before a control thread can join and release the buffers.
BOOL KSEngine_Stop(KSEngine*);
BOOL KSEngine_Cleanup(KSEngine*);
void KSEngine_GetStats(KSEngine*, KSStats*);
#ifdef __cplusplus
}
#endif
#endif
