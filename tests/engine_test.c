#include "../ks_engine.h"
#include <setupapi.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

// PACED simulates a real-time device: output packets play one buffer period each,
// and every played period records one input buffer, delivered in blocks of
// deliveryBlock buffers after startBacklog buffers recorded before output starts.
enum Mode { ASYNC_OK, SYNC_OK, SUBMIT_FAIL, COMPLETE_FAIL, WAIT_TIMEOUT_MODE,
    WAIT_FAILED_MODE, SHORT_READ, INVALID_READ, STATE_FAIL, HOLD_CALLBACK, STOP_IN_PREPARE,
    REQUEUE_FAIL, WAIT_FOR_STOP, PACED };
typedef struct {
    OVERLAPPED* ov;
    KSSTREAM_HEADER* header;
    HANDLE pin;
    unsigned sequence; // PACED capture requests, in submission order.
    BOOL active;
    BOOL cancelled;
    BOOL completed;
} Request;
static enum Mode mode;
#define MAX_REQUESTS (KS_MAX_RENDER_PACKETS + KS_MAX_CAPTURE_PACKETS)
static Request requests[MAX_REQUESTS];
static KSEngine* activeEngine;
static unsigned submissions, completions, callbacks, pendingReuse, runWithoutPriming, errorCallbacks;
static unsigned captureSequence, recordedBuffers, consumed[256];
static unsigned played, deliveryBlock = 1, startBacklog, stopAfter = 12;
static ULONG extraOutput;
static unsigned renderSequence;
static BOOL stallDrainsOutput = TRUE; // FALSE: only the oldest queued output packet plays.
static int stallRecords = -1; // Buffers recorded during a stall; -1 records for the whole stall.
static BYTE preparedInput[2][KS_MAX_BUFFER_SAMPLES * 4];
static unsigned preparations, stopPolls, maxSkipsPerCallback;
static LONG64 skipsSeen;
static int stallAt = -1;
static int stallPeriods = 5;
static LONGLONG captureSubmitCost;
static unsigned failCaptureSubmission, failCaptureCompletion;
static BOOL rejectHint, asyncHint;
static unsigned hintCalls;
static OVERLAPPED* pendingHint;
static ULONG* hintValue;
static BOOL failThread, failedJoin, delayCancellation, fakeClock;
static LONGLONG fakeNow, fakePeriod;
static HANDLE callbackEntered, releaseCallback, drainEntered, releaseDrain, stopFinished;
static const HANDLE renderPin = (HANDLE)(uintptr_t)0x1000;
static const HANDLE capturePin = (HANDLE)(uintptr_t)0x2000;
static const HANDLE filter = (HANDLE)(uintptr_t)0x3000;
static int pinCreates;

static Request* FindRequest(OVERLAPPED* ov) {
    for (int i = 0; i < MAX_REQUESTS; ++i) if (requests[i].ov == ov) return &requests[i];
    for (int i = 0; i < MAX_REQUESTS; ++i) if (!requests[i].ov) { requests[i].ov = ov; return &requests[i]; }
    assert(0);
    return NULL;
}
static void CompleteRequest(Request* request) {
    request->completed = TRUE;
    request->ov->Internal = 0;
    SetEvent(request->ov->hEvent);
}
static void CompleteRecorded(void) {
    for (int i = 0; i < MAX_REQUESTS; ++i)
        if (requests[i].active && !requests[i].completed && requests[i].pin == capturePin &&
            requests[i].sequence <= recordedBuffers) CompleteRequest(&requests[i]);
}
// The device records for some periods; input becomes available block by block.
static void Record(unsigned periods) {
    played += periods;
    recordedBuffers = startBacklog + played / deliveryBlock * deliveryBlock;
    CompleteRecorded();
}
// PACED: waiting for an output packet lets the device play it, which records one input buffer.
static void AdvanceDevice(HANDLE event) {
    for (int i = 0; i < MAX_REQUESTS; ++i) {
        Request* request = &requests[i];
        if (!request->active || request->ov->hEvent != event || request->completed) continue;
        assert(request->pin == renderPin); // Input is never awaited before it is recorded.
        fakeNow += fakePeriod;
        CompleteRequest(request);
        Record(1);
    }
}
static BOOL WINAPI TestIoctl(HANDLE pin, DWORD code, LPVOID in, DWORD ins,
                            LPVOID out, DWORD outs, LPDWORD bytes, LPOVERLAPPED ov) {
    (void)ins;
    assert(ov && ov->hEvent);
    if (code == IOCTL_KS_PROPERTY) {
        KSPROPERTY* prop = in;
        if (prop->Set.Data1 == 0x4e73c07f) {
            // Windows SDK ksmedia.h values; older MinGW releases omit these.
            const GUID expected = {0x4e73c07f, 0x23cc, 0x4955, {0xa7, 0xea, 0x3d, 0xa5, 0x02, 0x49, 0x62, 0x90}};
            assert(IsEqualGUID(&prop->Set, &expected));
            assert(pin == capturePin && prop->Id == 1 && prop->Flags == KSPROPERTY_TYPE_SET);
            assert(ins == sizeof(KSPROPERTY) && outs == sizeof(ULONG));
            assert(*(ULONG*)out >= 3 && *(ULONG*)out <= 10);
            ++hintCalls;
            if (asyncHint) {
                pendingHint = ov;
                hintValue = out;
                SetEvent(ov->hEvent);
                SetLastError(ERROR_IO_PENDING);
                return FALSE;
            }
            if (rejectHint) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
            return TRUE;
        }
        if (prop->Set.Data1 == 0x8c134960) {
            if (prop->Id == KSPROPERTY_PIN_CTYPES) {
                assert(outs == sizeof(ULONG));
                *(ULONG*)out = 12;
            } else if (prop->Id == KSPROPERTY_PIN_DATAFLOW) {
                *(KSPIN_DATAFLOW*)out = ((KSP_PIN*)in)->PinId == 11 ? KSPIN_DATAFLOW_OUT : KSPIN_DATAFLOW_IN;
            } else if (prop->Id == KSPROPERTY_PIN_COMMUNICATION) {
                *(KSPIN_COMMUNICATION*)out = ((KSP_PIN*)in)->PinId >= 10 ? KSPIN_COMMUNICATION_SINK : KSPIN_COMMUNICATION_BRIDGE;
            } else assert(0);
            return TRUE;
        }
        assert(prop->Set.Data1 == 0x1d58c920 && outs == sizeof(KSSTATE));
        KSSTATE state = *(KSSTATE*)out;
        if (state == KSSTATE_RUN && submissions != activeEngine->renderPackets + activeEngine->capturePackets)
            ++runWithoutPriming;
        if (mode == STATE_FAIL && state == KSSTATE_PAUSE) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
        return TRUE;
    }
    assert(code == IOCTL_KS_READ_STREAM || code == IOCTL_KS_WRITE_STREAM);
    assert(pin == renderPin || pin == capturePin);
    assert(out == in && outs == sizeof(KSSTREAM_HEADER));
    ++submissions;
    Request* request = FindRequest(ov);
    if (request->active) ++pendingReuse;
    assert(!request->active);
    if (mode == SUBMIT_FAIL ||
        (mode == REQUEUE_FAIL && submissions > activeEngine->renderPackets + activeEngine->capturePackets) ||
        (mode == PACED && pin == capturePin && captureSequence + 1 == failCaptureSubmission)) {
        SetLastError(ERROR_DEVICE_NOT_CONNECTED);
        return FALSE;
    }
    request->header = out;
    assert(request->header->OptionsFlags == 0);
    if (code == IOCTL_KS_READ_STREAM) {
        memset(request->header->Data, 0x33, request->header->FrameExtent);
        request->header->DataUsed = mode == SHORT_READ ? request->header->FrameExtent / 2 :
                                   mode == INVALID_READ ? request->header->FrameExtent + 4 : request->header->FrameExtent;
    }
    *bytes = request->header->FrameExtent;
    if (mode == SYNC_OK) return TRUE;
    request->active = TRUE;
    request->cancelled = FALSE;
    if (mode == PACED) {
        request->pin = pin;
        request->completed = FALSE;
        ov->Internal = STATUS_PENDING;
        if (code == IOCTL_KS_WRITE_STREAM) request->sequence = ++renderSequence; // Play order.
        if (code == IOCTL_KS_READ_STREAM) {
            if (callbacks) fakeNow += captureSubmitCost;
            request->sequence = ++captureSequence;
            memcpy(request->header->Data, &request->sequence, sizeof(request->sequence));
            CompleteRecorded(); // Input recorded earlier is delivered at once.
        }
    } else if (mode != WAIT_TIMEOUT_MODE && mode != WAIT_FAILED_MODE && mode != WAIT_FOR_STOP) SetEvent(ov->hEvent);
    SetLastError(ERROR_IO_PENDING);
    return FALSE;
}
static BOOL WINAPI TestResult(HANDLE pin, LPOVERLAPPED ov, LPDWORD bytes, BOOL wait) {
    if (ov == pendingHint) {
        assert(pin == capturePin && wait && *hintValue >= 3 && *hintValue <= 10);
        pendingHint = NULL;
        *bytes = sizeof(ULONG);
        if (rejectHint) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
        return TRUE;
    }
    (void)pin;
    Request* request = FindRequest(ov);
    assert(request->active);
    if (mode == PACED && !request->cancelled) assert(request->completed && !wait);
    if (request->cancelled && delayCancellation) {
        assert(wait);
        SetEvent(drainEntered);
        assert(WaitForSingleObject(releaseDrain, 5000) == WAIT_OBJECT_0);
    }
    request->active = FALSE;
    ++completions;
    *bytes = request->header->FrameExtent;
    if (request->cancelled) { SetLastError(ERROR_OPERATION_ABORTED); return FALSE; }
    if (mode == COMPLETE_FAIL || (mode == PACED && pin == capturePin && request->sequence == failCaptureCompletion)) {
        SetLastError(ERROR_DEVICE_NOT_CONNECTED);
        return FALSE;
    }
    return TRUE;
}
static BOOL WINAPI TestCancel(HANDLE pin, LPOVERLAPPED ov) {
    (void)pin; (void)ov;
    for (int i = 0; i < MAX_REQUESTS; ++i) if (requests[i].active) requests[i].cancelled = TRUE;
    return TRUE; // Completion is separate, and can be deliberately delayed.
}
static DWORD WINAPI TestWaitMany(DWORD count, const HANDLE* handles, BOOL all, DWORD ms) {
    if (ms == 500 && mode == WAIT_TIMEOUT_MODE) return WAIT_TIMEOUT;
    if (ms == 500 && mode == WAIT_FAILED_MODE) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    if (ms == 500 && mode == WAIT_FOR_STOP) SetEvent(callbackEntered);
    if (mode == PACED && count == 2) AdvanceDevice(handles[1]);
    return WaitForMultipleObjects(count, handles, all, ms);
}
static BOOL WINAPI TestCounter(LARGE_INTEGER* now) {
    if (!fakeClock) return QueryPerformanceCounter(now);
    now->QuadPart = fakeNow;
    return TRUE;
}
static DWORD WINAPI TestWaitOne(HANDLE handle, DWORD ms) {
    if (activeEngine && handle == activeEngine->hStopEvent && ms == 0) ++stopPolls;
    if (failedJoin && handle == activeEngine->hThread) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    return WaitForSingleObject(handle, ms);
}
static BOOL WINAPI TestClose(HANDLE handle) {
    if (handle == renderPin || handle == capturePin || handle == filter) return TRUE;
    return CloseHandle(handle);
}
static BOOL WINAPI TestFree(LPVOID data, SIZE_T size, DWORD type) {
    for (int i = 0; i < MAX_REQUESTS; ++i) assert(!requests[i].active);
    return VirtualFree(data, size, type);
}
static HMODULE WINAPI TestLoadLibrary(LPCWSTR name) {
    if (wcscmp(name, L"avrt.dll") == 0) return NULL;
    return LoadLibraryW(name);
}
static BOOL WINAPI TestPriority(HANDLE handle, int priority) { (void)handle; (void)priority; return TRUE; }
static HANDLE WINAPI TestCreateThread(LPSECURITY_ATTRIBUTES attr, SIZE_T stack,
    LPTHREAD_START_ROUTINE fn, LPVOID param, DWORD flags, LPDWORD id) {
    if (failThread) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    return CreateThread(attr, stack, fn, param, flags, id);
}
#define DeviceIoControl TestIoctl
#define GetOverlappedResult TestResult
#define CancelIoEx TestCancel
#define WaitForMultipleObjects TestWaitMany
#define WaitForSingleObject TestWaitOne
#define CloseHandle TestClose
#define VirtualFree TestFree
#define LoadLibraryW TestLoadLibrary
#define SetThreadPriority TestPriority
#define CreateThread TestCreateThread
#define QueryPerformanceCounter TestCounter
#include "../ks_engine.c"

static void PrepareInput(void* context, long index, const void* input) {
    assert(context == activeEngine && input);
    assert(index == (long)(callbacks % 2));
    ++preparations;
    memcpy(preparedInput[index], input, activeEngine->bufferSizeBytes);
    if (mode == SHORT_READ) {
        const BYTE* bytes = input;
        for (ULONG i = 0; i < activeEngine->bufferSizeBytes; ++i)
            assert(bytes[i] == (i < activeEngine->bufferSizeBytes / 2 ? 0x33 : 0));
    }
    if (mode == STOP_IN_PREPARE) KSEngine_RequestStop(activeEngine);
}
static void CheckCaptureRequeued(void) {
    if (mode != SYNC_OK)
        for (ULONG i = 0; i < activeEngine->capturePackets; ++i) assert(activeEngine->capture[i].pending);
    assert(preparations == callbacks + 1);
    LONG64 skips = activeEngine->inputSkips + activeEngine->inputTrims;
    unsigned skipped = (unsigned)(skips - skipsSeen);
    if (skipped > maxSkipsPerCallback) maxSkipsPerCallback = skipped;
    assert(skipped <= KS_MAX_RECOVERY_SKIPS);
    skipsSeen = skips;
}
static void Callback(void* context, long index, void* output, ULONGLONG timestamp) {
    assert(context == activeEngine && output && timestamp);
    assert(index == (long)(callbacks % 2));
    CheckCaptureRequeued();
    ++callbacks;
    if (mode == HOLD_CALLBACK) {
        SetEvent(callbackEntered);
        assert(WaitForSingleObject(releaseCallback, 5000) == WAIT_OBJECT_0);
    } else if (callbacks == 4) KSEngine_RequestStop(activeEngine);
}
static void PacedCallback(void* context, long index, void* output, ULONGLONG timestamp) {
    (void)output;
    assert(context == activeEngine && timestamp && index == (long)(callbacks % 2));
    CheckCaptureRequeued();
    memcpy(&consumed[callbacks], preparedInput[index], sizeof(consumed[0]));
    if ((int)callbacks == stallAt) {
        // During a host stall the queued output packets play,
        // then output runs dry while the device keeps recording.
        fakeNow += stallPeriods * fakePeriod;
        Request* oldest = NULL;
        for (int i = 0; i < MAX_REQUESTS; ++i) {
            Request* request = &requests[i];
            if (!request->active || request->completed || request->pin != renderPin) continue;
            if (stallDrainsOutput) CompleteRequest(request);
            else if (!oldest || request->sequence < oldest->sequence) oldest = request;
        }
        if (oldest) CompleteRequest(oldest);
        Record(stallRecords < 0 ? (unsigned)stallPeriods : (unsigned)stallRecords);
    }
    if (++callbacks == stopAfter) KSEngine_RequestStop(activeEngine);
}
static void OnError(void* context, DWORD error) {
    assert(context == activeEngine && error != ERROR_SUCCESS);
    ++errorCallbacks;
}
static void SetupSized(KSEngine* engine, enum Mode selectedMode, ULONG rate, ULONG samples) {
    memset(engine, 0, sizeof(*engine));
    memset(requests, 0, sizeof(requests));
    memset(consumed, 0, sizeof(consumed));
    submissions = completions = callbacks = pendingReuse = runWithoutPriming = errorCallbacks = 0;
    captureSequence = played = renderSequence = 0;
    recordedBuffers = startBacklog;
    preparations = stopPolls = maxSkipsPerCallback = 0;
    skipsSeen = 0;
    mode = selectedMode;
    activeEngine = engine;
    engine->hFilter = filter;
    engine->hRenderPin = renderPin;
    engine->hCapturePin = capturePin;
    engine->channels = 2;
    engine->bitsPerSample = 16;
    engine->sampleRate = rate;
    engine->bufferSizeSamples = samples;
    engine->bufferSizeBytes = samples * 4;
    QueryPerformanceFrequency(&engine->counterFrequency);
    engine->hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    engine->hReadyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    engine->hLaunchEvent = CreateEventW(NULL, TRUE, TRUE, NULL);
    engine->onBufferReady = Callback;
    engine->onInputReady = PrepareInput;
    engine->onError = OnError;
    engine->callbackContext = engine;
    engine->queuedInputSamples = -1; // As KSEngine_Start leaves it.
    engine->renderPackets = KS_MIN_PACKETS + extraOutput;
    engine->capturePackets = CapturePacketsFor(samples, rate);
    for (ULONG p = 0; p < KS_MAX_RENDER_PACKETS + engine->capturePackets; ++p) { // Same allocation as Init.
        KSPacket* packet = Packet(engine, p);
        packet->data = VirtualAlloc(NULL, engine->bufferSizeBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        packet->event = CreateEventW(NULL, TRUE, FALSE, NULL);
        assert(packet->data && packet->event);
    }
}
// Runs the paced device with a fake clock until stopAfter callbacks.
static void RunPaced(KSEngine* engine, ULONG rate, ULONG samples) {
    SetupSized(engine, PACED, rate, samples);
    fakeClock = TRUE;
    fakeNow = 1000000000;
    fakePeriod = engine->counterFrequency.QuadPart * samples / rate;
    engine->onBufferReady = PacedCallback;
    AudioStreamingThread(engine);
    fakeClock = FALSE;
    assert(engine->lastError == 0 && callbacks == stopAfter && errorCallbacks == 0 && !pendingReuse && !runWithoutPriming);
    for (int i = 0; i < MAX_REQUESTS; ++i) assert(!requests[i].active);
}
// Consumed input only moves forward, skipping exactly the buffers the engine dropped.
static void CheckConsumedSequence(const KSEngine* engine) {
    unsigned gaps = 0;
    for (unsigned i = 1; i < callbacks; ++i) {
        assert(consumed[i] > consumed[i - 1]);
        gaps += consumed[i] - consumed[i - 1] - 1;
    }
    assert(consumed[0] == 1 && gaps == engine->inputSkips + engine->inputTrims);
}
static void Setup(KSEngine* engine, enum Mode selectedMode) {
    SetupSized(engine, selectedMode, 48000, 96);
}
static GUID SdkGuid(unsigned long d1, unsigned short d2, unsigned short d3, unsigned char b0, unsigned char b1,
                    unsigned char b2, unsigned char b3, unsigned char b4, unsigned char b5, unsigned char b6,
                    unsigned char b7) {
    GUID guid = {d1, d2, d3, {b0, b1, b2, b3, b4, b5, b6, b7}};
    return guid;
}
static DWORD WINAPI CreateTestPin(HANDLE handle, PKSPIN_CONNECT connect, ACCESS_MASK access, PHANDLE pin) {
    assert(handle == filter);
    ++pinCreates;
    assert(connect->PinId == (access == GENERIC_WRITE ? 10U : 11U));
    KSDATAFORMAT_WAVEFORMATEX* format = (KSDATAFORMAT_WAVEFORMATEX*)(connect + 1);
    // Compare against the SDK definitions: a wrong major type makes real
    // devices reject every format with ERROR_NO_MATCH.
    GUID audio = SdkGuid(STATIC_KSDATAFORMAT_TYPE_AUDIO);
    GUID pcm = SdkGuid(STATIC_KSDATAFORMAT_SUBTYPE_PCM);
    GUID waveFormatEx = SdkGuid(STATIC_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX);
    assert(IsEqualGUID(&format->DataFormat.MajorFormat, &audio));
    assert(IsEqualGUID(&format->DataFormat.SubFormat, &pcm));
    assert(IsEqualGUID(&format->DataFormat.Specifier, &waveFormatEx));
    assert(format->WaveFormatEx.nSamplesPerSec == 48000 && format->WaveFormatEx.nChannels == 2);
    assert(format->WaveFormatEx.wBitsPerSample == 16);
    *pin = access == GENERIC_WRITE ? renderPin : capturePin;
    return ERROR_SUCCESS;
}
// How KsCreatePin answers when another program holds the pin, or the format is rejected.
static DWORD WINAPI BusyPin(HANDLE handle, PKSPIN_CONNECT connect, ACCESS_MASK access, PHANDLE pin) {
    (void)handle; (void)connect; (void)access; (void)pin;
    return ERROR_NO_SYSTEM_RESOURCES;
}
static DWORD WINAPI RejectingPin(HANDLE handle, PKSPIN_CONNECT connect, ACCESS_MASK access, PHANDLE pin) {
    (void)handle; (void)connect; (void)access; (void)pin;
    return ERROR_NO_MATCH;
}
static DWORD WINAPI StopOnControlThread(void* context) {
    assert(KSEngine_Stop(context));
    SetEvent(stopFinished);
    return 0;
}

int main(void) {
    KSEngine engine;
    assert(!KSEngine_Init(&engine, 48000, 2, 16, 0, NULL));
    assert(!KSEngine_Init(&engine, 48000, 2, 16, 1024, NULL));
    assert(!KSEngine_Init(&engine, 96000, 2, 16, 96, NULL));
    assert(IsTargetDevice(L"\\\\?\\USB#VID_08BB&PID_2902&MI_00#123"));
    assert(!IsTargetDevice(L"\\\\?\\USB#VID_1234&PID_2902#123"));
    assert(!IsTargetDevice(L"\\\\?\\USB#VID_08BB&PID_29020#123"));
    const WCHAR* umc = L"\\\\?\\USB#VID_08BB&PID_2902&MI_00#7&1A2B&0&0000#{guid}";
    const WCHAR* umcId = L"USB\\VID_08BB&PID_2902&MI_00\\7&1A2B&0&0000";
    const WCHAR* other = L"\\\\?\\USB#VID_1234&PID_0001&MI_00#8&99#{guid}";
    const WCHAR* otherId = L"USB\\VID_1234&PID_0001&MI_00\\8&99";
    assert(IsSelectedDevice(NULL, umc, L"USB Audio CODEC", umcId));
    assert(IsSelectedDevice(L"", umc, L"", L""));
    assert(!IsSelectedDevice(L"", other, L"USB Audio CODEC", otherId));
    assert(IsSelectedDevice(L"usb audio codec", other, L"USB Audio CODEC", otherId));
    assert(IsSelectedDevice(L"Audio", other, L"USB Audio CODEC", otherId));
    assert(!IsSelectedDevice(L"UMC404", umc, L"USB Audio CODEC", umcId));
    assert(IsSelectedDevice(L"7&1a2b", umc, L"USB Audio CODEC", umcId));
    assert(!IsSelectedDevice(L"7&1a2b", other, L"USB Audio CODEC", otherId));
    // Enumerates this machine's real audio devices, but none can match this name.
    assert(!KSEngine_Init(&engine, 48000, 2, 16, 96, L"no device is called this 7f3a") &&
           GetLastError() == KS_ERROR_DEVICE_NOT_FOUND);
    Setup(&engine, ASYNC_OK);
    assert(CreateAudioPin(&engine, CreateTestPin, KSPIN_DATAFLOW_IN) == renderPin);
    assert(CreateAudioPin(&engine, CreateTestPin, KSPIN_DATAFLOW_OUT) == capturePin);
    assert(pinCreates == 2);
    assert(!CreateAudioPin(&engine, BusyPin, KSPIN_DATAFLOW_IN) && GetLastError() == KS_ERROR_DEVICE_IN_USE);
    assert(!CreateAudioPin(&engine, RejectingPin, KSPIN_DATAFLOW_OUT) && GetLastError() == KS_ERROR_FORMAT_UNSUPPORTED);
    assert(KSEngine_Cleanup(&engine));

    // A capture-duration hint is optional, sent to the capture pin while stopped.
    // Both synchronous and overlapped property requests preserve the latency
    // estimate and do not turn rejection into a streaming error.
    for (int asynchronous = 0; asynchronous < 2; ++asynchronous) {
        for (int rejected = 0; rejected < 2; ++rejected) {
            Setup(&engine, ASYNC_OK);
            engine.inputDelaySamples = 480;
            asyncHint = asynchronous;
            rejectHint = rejected;
            unsigned beforeHints = hintCalls;
            assert(KSEngine_SetCaptureBufferDuration(&engine, 0) && hintCalls == beforeHints);
            assert(!KSEngine_SetCaptureBufferDuration(&engine, 2));
            assert(!KSEngine_SetCaptureBufferDuration(&engine, 11));
            assert(KSEngine_SetCaptureBufferDuration(&engine, 3) == !rejected);
            assert(hintCalls == beforeHints + 1 && !pendingHint && engine.captureBufferMs == 3);
            assert(engine.captureBufferError == (rejected ? ERROR_NOT_SUPPORTED : ERROR_SUCCESS));
            assert(engine.inputDelaySamples == 480 && !engine.lastError && !engine.ioErrors);
            AudioStreamingThread(&engine); // Unsupported hint still permits streaming.
            assert(callbacks == 4 && !engine.lastError && !stopPolls);
            assert(KSEngine_Cleanup(&engine));
        }
    }
    asyncHint = rejectHint = FALSE;

    // A real-time device: no dropouts, no skipped input. Then a host stall makes
    // output run dry while input keeps recording. The engine must count one dropout,
    // measure that output now plays four buffers later, and skip already-recorded
    // input (keeping at most one buffer plus timing noise) instead of lagging behind.
    const ULONG rates[] = {44100, 48000};
    for (unsigned rate = 0; rate < 2; ++rate) {
        for (ULONG samples = KS_MIN_BUFFER_SAMPLES; samples <= KS_MAX_BUFFER_SAMPLES; samples += KS_BUFFER_GRANULARITY) {
            for (int stall = 0; stall < 2; ++stall) {
                SetupSized(&engine, PACED, rates[rate], samples);
                fakeClock = TRUE;
                fakeNow = 1000000000;
                fakePeriod = engine.counterFrequency.QuadPart * samples / rates[rate];
                engine.onBufferReady = PacedCallback;
                stallAt = stall ? 3 : -1;
                AudioStreamingThread(&engine);
                fakeClock = FALSE;
                stallAt = -1;
                assert(engine.lastError == 0 && callbacks == 12 && errorCallbacks == 0 && !pendingReuse && !runWithoutPriming);
                for (int i = 0; i < MAX_REQUESTS; ++i) assert(!requests[i].active);
                static const unsigned steady[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
                static const unsigned resynced[] = {1, 2, 3, 4, 5, 6, 10, 11, 12, 13, 14, 15};
                if (!stall) {
                    assert(engine.dropouts == 0 && engine.inputSkips == 0 && engine.slipSamples == 0);
                    assert(memcmp(consumed, steady, sizeof(steady)) == 0);
                } else {
                    assert(engine.dropouts == 1 && engine.inputSkips == 3 && engine.callbackOverruns == 1);
                    assert(maxSkipsPerCallback == 3); // Catch up in one callback, not three.
                    assert(engine.slipSamples == 4 * fakePeriod * rates[rate] / engine.counterFrequency.QuadPart);
                    assert(memcmp(consumed, resynced, sizeof(resynced)) == 0);
                }
                assert(KSEngine_Cleanup(&engine));
            }
        }
    }

    // A much longer stall must respect both the count and time recovery bounds.
    for (int slowSubmissions = 0; slowSubmissions < 2; ++slowSubmissions) {
        Setup(&engine, PACED);
        fakeClock = TRUE;
        fakeNow = 1000000000;
        fakePeriod = engine.counterFrequency.QuadPart * 96 / 48000;
        captureSubmitCost = slowSubmissions ? engine.counterFrequency.QuadPart / 5000 : 0; // 200 us.
        stallPeriods = 20;
        stallAt = 3;
        engine.onBufferReady = PacedCallback;
        AudioStreamingThread(&engine);
        assert(callbacks == 12 && !engine.lastError && !pendingReuse && !stopPolls);
        assert(maxSkipsPerCallback == (slowSubmissions ? 1U : KS_MAX_RECOVERY_SKIPS));
        assert(KSEngine_Cleanup(&engine));
    }
    fakeClock = FALSE;
    captureSubmitCost = 0;
    stallPeriods = 5;
    stallAt = -1;

    // Capture queue depth follows the buffer size: 8 ms of visible input plus the current buffer.
    assert(CapturePacketsFor(48, 48000) == 9 && CapturePacketsFor(48, 44100) == 9);
    assert(CapturePacketsFor(96, 48000) == 5 && CapturePacketsFor(192, 48000) == 3);
    assert(CapturePacketsFor(384, 48000) == 2 && CapturePacketsFor(1008, 48000) == 2);

    // Extra output buffers deepen only the output queue; the host's double buffer
    // still alternates. A stall that empties the deeper queue for four periods is
    // one dropout, measured as four periods, and input resyncs by three buffers.
    for (ULONG extra = 1; extra <= KS_MAX_EXTRA_OUTPUT_BUFFERS; ++extra) {
        for (int stall = 0; stall < 2; ++stall) {
            extraOutput = extra;
            stallPeriods = 5 + (int)extra;
            stallAt = stall ? 3 : -1;
            RunPaced(&engine, 48000, 96);
            assert(engine.renderPackets == KS_MIN_PACKETS + extra);
            CheckConsumedSequence(&engine);
            if (!stall) {
                assert(engine.dropouts == 0 && engine.inputSkips == 0 && consumed[11] == 12);
            } else {
                assert(engine.dropouts == 1 && engine.inputSkips == 3);
                assert(engine.slipSamples == 4 * fakePeriod * 48000 / engine.counterFrequency.QuadPart);
                // The delay is measured once the whole queue has refilled (callback 3 + queue
                // depth); the three stale buffers are skipped at the next callback.
                unsigned resync = 3 + KS_MIN_PACKETS + extra + 1;
                assert(consumed[resync] - consumed[resync - 1] == 4);
            }
            assert(KSEngine_Cleanup(&engine));
        }
    }
    // With one extra output buffer, a host one period late lets one queued output
    // packet play but leaves another queued: no dropout and no skipped input.
    extraOutput = 1;
    stallPeriods = 1;
    stallAt = 3;
    stallDrainsOutput = FALSE;
    RunPaced(&engine, 48000, 96);
    CheckConsumedSequence(&engine);
    assert(engine.dropouts == 0 && engine.inputSkips == 0 && engine.inputTrims == 0 && consumed[11] == 12);
    assert(KSEngine_Cleanup(&engine));
    stallDrainsOutput = TRUE;
    extraOutput = 0;
    stallPeriods = 5;
    stallAt = -1;
    Setup(&engine, ASYNC_OK);
    assert(KSEngine_SetExtraOutputBuffers(&engine, KS_MAX_EXTRA_OUTPUT_BUFFERS) && engine.renderPackets == KS_MAX_RENDER_PACKETS);
    assert(!KSEngine_SetExtraOutputBuffers(&engine, KS_MAX_EXTRA_OUTPUT_BUFFERS + 1) && engine.renderPackets == KS_MAX_RENDER_PACKETS);
    assert(KSEngine_SetExtraOutputBuffers(&engine, 0) && engine.renderPackets == KS_MIN_PACKETS);
    assert(KSEngine_Cleanup(&engine));
    KSEngine unopened;
    memset(&unopened, 0, sizeof(unopened));
    assert(!KSEngine_SetExtraOutputBuffers(&unopened, 1)); // Before Init allocates the packets.

    // Input alignment trims input that waited longer than needed, down to the
    // reserve, within a window or two, and never waits for input (AdvanceDevice
    // asserts that). Input that arrives just in time, like Windows' default 10 ms
    // blocks, is left alone.
    struct { ULONG samples; unsigned block, backlog, trimmed, reserve, queued; } aligned[] = {
        {48, 1, 6, 4, 2, 2},   // 1 ms buffers, input every buffer, 6 ms queued: keep 2 ms.
        {48, 3, 9, 5, 2, 2},   // 3 ms blocks: the minimum before each block is what counts.
        {48, 10, 10, 0, 2, 1}, // 10 ms blocks arriving just in time: nothing to trim.
        {96, 1, 4, 3, 1, 1},   // 2 ms buffers: the reserve is one buffer.
    };
    for (unsigned c = 0; c < sizeof(aligned) / sizeof(aligned[0]); ++c) {
        deliveryBlock = aligned[c].block;
        startBacklog = aligned[c].backlog;
        stopAfter = 200;
        RunPaced(&engine, 48000, aligned[c].samples);
        CheckConsumedSequence(&engine);
        assert(engine.dropouts == 0 && engine.inputSkips == 0 && engine.inputTrims == aligned[c].trimmed);
        assert(engine.inputReserve == (LONG)aligned[c].reserve);
        assert(engine.queuedInputSamples == (LONG)(aligned[c].queued * aligned[c].samples));
        assert(KSEngine_Cleanup(&engine));
    }
    // A dropout right after a trim keeps one more buffer in reserve.
    deliveryBlock = 1;
    startBacklog = 6;
    stallAt = 42; // The trim happens at callback 41.
    RunPaced(&engine, 48000, 48);
    CheckConsumedSequence(&engine);
    assert(engine.inputTrims == 4 && engine.dropouts == 1 && engine.inputReserve == 3);
    assert(KSEngine_Cleanup(&engine));
    // A stall after which output resumes later but the device recorded only what the
    // immediate catch-up callbacks need: the delay measured on output never becomes
    // queued input. The engine must not wait for input it is owed, and must stop
    // expecting it after two windows so that input alignment resumes.
    startBacklog = 0;
    stallAt = 3;
    stallRecords = 1;
    RunPaced(&engine, 48000, 48);
    CheckConsumedSequence(&engine);
    assert(engine.dropouts == 1 && engine.inputSkips == 0 && engine.queuedInputSamples == 0);
    assert(KSEngine_Cleanup(&engine));
    stallRecords = -1;
    deliveryBlock = 1;
    startBacklog = 0;
    stopAfter = 12;
    stallAt = -1;

    // An I/O failure inside the recovery loop must stop the stream immediately,
    // without delivering another host callback or reusing a pending packet.
    for (int failCompletion = 0; failCompletion < 2; ++failCompletion) {
        Setup(&engine, PACED);
        fakeClock = TRUE;
        fakeNow = 1000000000;
        fakePeriod = engine.counterFrequency.QuadPart * 96 / 48000;
        stallAt = 3;
        // Both fail at the second recovery skip, in callback 6: the resubmission of
        // input buffer 8 (after the primed and the six requeued requests), or the
        // completion of input buffer 9.
        failCaptureSubmission = failCompletion ? 0 : engine.capturePackets + 8;
        failCaptureCompletion = failCompletion ? 9 : 0;
        engine.onBufferReady = PacedCallback;
        AudioStreamingThread(&engine);
        assert(callbacks == 6 && engine.lastError == ERROR_DEVICE_NOT_CONNECTED);
        assert(errorCallbacks == 1 && !pendingReuse);
        assert(KSEngine_Cleanup(&engine));
    }
    failCaptureSubmission = failCaptureCompletion = 0;
    fakeClock = FALSE;
    stallAt = -1;

    Setup(&engine, STOP_IN_PREPARE);
    AudioStreamingThread(&engine);
    assert(preparations == 1 && callbacks == 0 && !engine.lastError && !pendingReuse);
    assert(KSEngine_Cleanup(&engine));

    Setup(&engine, REQUEUE_FAIL);
    AudioStreamingThread(&engine);
    assert(preparations == 1 && callbacks == 0 && engine.lastError == ERROR_DEVICE_NOT_CONNECTED);
    assert(errorCallbacks == 1 && !pendingReuse);
    assert(KSEngine_Cleanup(&engine));

    const enum Mode modes[] = {ASYNC_OK, SYNC_OK, SUBMIT_FAIL, COMPLETE_FAIL, WAIT_TIMEOUT_MODE,
        WAIT_FAILED_MODE, SHORT_READ, INVALID_READ, STATE_FAIL};
    const DWORD errors[] = {0, 0, ERROR_DEVICE_NOT_CONNECTED, ERROR_DEVICE_NOT_CONNECTED, ERROR_TIMEOUT,
        ERROR_INVALID_HANDLE, 0, ERROR_INVALID_DATA, ERROR_ACCESS_DENIED};
    for (unsigned t = 0; t < sizeof(modes) / sizeof(modes[0]); ++t) {
        Setup(&engine, modes[t]);
        AudioStreamingThread(&engine);
        assert(engine.lastError == (LONG)errors[t]);
        assert(callbacks == (errors[t] ? 0U : 4U));
        assert(!pendingReuse && !runWithoutPriming);
        assert(errorCallbacks == (errors[t] ? 1U : 0U));
        for (int i = 0; i < MAX_REQUESTS; ++i) assert(!requests[i].active);
        assert(engine.waitTimeouts == (modes[t] == WAIT_TIMEOUT_MODE ? 1 : 0));
        assert(engine.shortReads == (modes[t] == SHORT_READ ? 4 : 0));
        assert(KSEngine_Cleanup(&engine));
    }
    Setup(&engine, ASYNC_OK);
    assert(!KSEngine_Start(&engine, NULL, Callback, OnError, &engine));
    assert(!KSEngine_Start(&engine, PrepareInput, NULL, OnError, &engine));
    assert(!engine.hThread && submissions == 0);
    failThread = TRUE;
    assert(!KSEngine_Start(&engine, PrepareInput, Callback, OnError, &engine));
    assert(!engine.hThread && !engine.isRunning && submissions == 0);
    failThread = FALSE;
    assert(KSEngine_Cleanup(&engine));

    // Exercise real thread/event synchronization, holding a host callback past
    // the old 1.5-second timeout, then delaying cancellation completion too.
    callbackEntered = CreateEventW(NULL, TRUE, FALSE, NULL);
    releaseCallback = CreateEventW(NULL, TRUE, FALSE, NULL);
    drainEntered = CreateEventW(NULL, TRUE, FALSE, NULL);
    releaseDrain = CreateEventW(NULL, TRUE, FALSE, NULL);
    stopFinished = CreateEventW(NULL, TRUE, FALSE, NULL);
    Setup(&engine, WAIT_FOR_STOP);
    assert(KSEngine_Start(&engine, PrepareInput, Callback, OnError, &engine));
    assert(WaitForSingleObject(callbackEntered, 5000) == WAIT_OBJECT_0);
    assert(!KSEngine_SetCaptureBufferDuration(&engine, 3));
    assert(!KSEngine_SetExtraOutputBuffers(&engine, 1)); // Not while streaming.
    KSEngine_RequestStop(&engine); // The event must still wake a blocked worker.
    assert(WaitForSingleObject(engine.hThread, 5000) == WAIT_OBJECT_0);
    assert(KSEngine_Stop(&engine) && !engine.lastError && !callbacks && !stopPolls);
    assert(KSEngine_Cleanup(&engine));
    ResetEvent(callbackEntered);
    Setup(&engine, HOLD_CALLBACK);
    assert(KSEngine_Start(&engine, PrepareInput, Callback, OnError, &engine));
    assert(WaitForSingleObject(callbackEntered, 5000) == WAIT_OBJECT_0);
    failedJoin = TRUE;
    BYTE* original = engine.render[0].data;
    assert(!KSEngine_Cleanup(&engine) && engine.render[0].data == original && engine.hThread);
    failedJoin = FALSE;
    delayCancellation = TRUE;
    HANDLE stopper = CreateThread(NULL, 0, StopOnControlThread, &engine, 0, NULL);
    assert(stopper);
    assert(WaitForSingleObject(stopFinished, 1600) == WAIT_TIMEOUT);
    SetEvent(releaseCallback);
    assert(WaitForSingleObject(drainEntered, 5000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(stopFinished, 0) == WAIT_TIMEOUT);
    assert(engine.render[0].data == original);
    SetEvent(releaseDrain);
    assert(WaitForSingleObject(stopper, 5000) == WAIT_OBJECT_0);
    CloseHandle(stopper);
    delayCancellation = FALSE;
    assert(!engine.hThread && !engine.isRunning && !pendingReuse);

    // Reuse the same allocated buffers for another start/stop cycle.
    ResetEvent(callbackEntered);
    ResetEvent(releaseCallback);
    callbacks = submissions = preparations = 0;
    memset(requests, 0, sizeof(requests));
    assert(KSEngine_Start(&engine, PrepareInput, Callback, OnError, &engine));
    assert(WaitForSingleObject(callbackEntered, 5000) == WAIT_OBJECT_0);
    KSEngine_RequestStop(&engine);
    SetEvent(releaseCallback);
    assert(KSEngine_Stop(&engine));
    assert(KSEngine_Cleanup(&engine));
    CloseHandle(callbackEntered);
    CloseHandle(releaseCallback);
    CloseHandle(drainEntered);
    CloseHandle(releaseDrain);
    CloseHandle(stopFinished);
    puts("PASS KS engine: capture hints, early requeue, all sizes/rates, bounded dropout resync, "
         "extra output buffers, input alignment, I/O failures, stop wakeup, cancellation, join safety and restart");
}
