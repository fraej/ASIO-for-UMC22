#include "ks_engine.h"
#include <setupapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// Older MinGW headers omit this Windows 7+ ksmedia.h property definition.
// https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/ksproperty-audio-buffer-duration
#ifndef KSPROPERTY_AUDIO_BUFFER_DURATION
#define KSPROPERTY_AUDIO_BUFFER_DURATION 1
#endif

static const GUID GUID_KSCATEGORY_RENDER = { 0x65e8773e, 0x8f56, 0x11d0, { 0xa3, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } };
static const GUID GUID_KSDATAFORMAT_TYPE_AUDIO = { 0x73647561, 0, 0x0010, { 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71 } };
static const GUID GUID_KSDATAFORMAT_SUBTYPE_PCM = { 1, 0, 0x0010, { 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71 } };
static const GUID GUID_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX = { 0x05589f81, 0xc356, 0x11ce, { 0xbf, 1, 0, 0xaa, 0, 0x55, 0x59, 0x5a } };
static const GUID GUID_KSPROPSETID_Connection = { 0x1d58c920, 0xac9b, 0x11cf, { 0xa5, 0xd6, 0x28, 0xdb, 4, 0xc1, 0, 0 } };
static const GUID GUID_KSPROPSETID_AudioBufferDuration = {
    0x4e73c07f, 0x23cc, 0x4955, { 0xa7, 0xea, 0x3d, 0xa5, 0x02, 0x49, 0x62, 0x90 }
};
static const GUID GUID_KSPROPSETID_Pin = { 0x8c134960, 0x51ad, 0x11cf, { 0x87, 0x8a, 0x94, 0xf8, 1, 0xc1, 0, 0 } };
static const GUID GUID_KSINTERFACESETID_Standard = { 0x1a8766a0, 0x62ce, 0x11cf, { 0xa5, 0xd6, 0x28, 0xdb, 4, 0xc1, 0, 0 } };
static const GUID GUID_KSMEDIUMSETID_Standard = { 0x4747b320, 0x62ce, 0x11cf, { 0xa5, 0xd6, 0x28, 0xdb, 4, 0xc1, 0, 0 } };

typedef DWORD (WINAPI *CreatePinFn)(HANDLE, PKSPIN_CONNECT, ACCESS_MASK, PHANDLE);
typedef HANDLE (WINAPI *SetCharacteristicsFn)(LPCSTR, LPDWORD);
typedef BOOL (WINAPI *SetPriorityFn)(HANDLE, int);
typedef BOOL (WINAPI *RevertCharacteristicsFn)(HANDLE);
#pragma pack(push, 1)
typedef struct {
    KSPIN_CONNECT connect;
    KSDATAFORMAT_WAVEFORMATEX format;
} PinRequest;
#pragma pack(pop)

static BOOL ValidHandle(HANDLE h) { return h && h != INVALID_HANDLE_VALUE; }

// All packets in one sequence: render first, then capture.
#define KS_ALL_PACKETS (KS_MAX_RENDER_PACKETS + KS_MAX_CAPTURE_PACKETS)
static KSPacket* Packet(KSEngine* engine, ULONG index) {
    return index < KS_MAX_RENDER_PACKETS ? &engine->render[index] : &engine->capture[index - KS_MAX_RENDER_PACKETS];
}

// Capture requests in flight: besides the one being processed, enough to see
// 8 ms of recorded input queued ahead, which input alignment can then trim.
static ULONG CapturePacketsFor(ULONG samples, ULONG rate) {
    ULONG packets = (rate / 125 + samples - 1) / samples + 1;
    return packets < KS_MIN_PACKETS ? KS_MIN_PACKETS : packets > KS_MAX_CAPTURE_PACKETS ? KS_MAX_CAPTURE_PACKETS : packets;
}

// Pin handles are asynchronous too. Keep control request storage alive until
// completion, even if an apparently simple property request returns pending.
static BOOL Control(HANDLE pin, DWORD code, void* in, DWORD inSize, void* out, DWORD outSize) {
    OVERLAPPED ov = {0};
    DWORD bytes = 0, error = ERROR_SUCCESS;
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return FALSE;
    BOOL ok = DeviceIoControl(pin, code, in, inSize, out, outSize, &bytes, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(pin, &ov, &bytes, TRUE);
    if (!ok) error = GetLastError();
    CloseHandle(ov.hEvent);
    SetLastError(error);
    return ok;
}

static BOOL SetPinState(HANDLE pin, KSSTATE state) {
    KSPROPERTY prop = {0};
    if (!ValidHandle(pin)) return TRUE;
    prop.Set = GUID_KSPROPSETID_Connection;
    prop.Id = KSPROPERTY_CONNECTION_STATE;
    prop.Flags = KSPROPERTY_TYPE_SET;
    return Control(pin, IOCTL_KS_PROPERTY, &prop, sizeof(prop), &state, sizeof(state));
}

static BOOL SetBothStates(KSEngine* engine, KSSTATE state) {
    return SetPinState(engine->hRenderPin, state) && SetPinState(engine->hCapturePin, state);
}

static BOOL PinProperty(HANDLE filter, ULONG pinId, ULONG id, void* out, DWORD size) {
    KSP_PIN prop = {0};
    prop.Property.Set = GUID_KSPROPSETID_Pin;
    prop.Property.Id = id;
    prop.Property.Flags = KSPROPERTY_TYPE_GET;
    prop.PinId = pinId;
    return Control(filter, IOCTL_KS_PROPERTY, &prop, sizeof(prop), out, size);
}

static HANDLE CreateAudioPin(KSEngine* engine, CreatePinFn createPin, KSPIN_DATAFLOW direction) {
    KSPROPERTY countProp = {0};
    ULONG count = 0;
    countProp.Set = GUID_KSPROPSETID_Pin;
    countProp.Id = KSPROPERTY_PIN_CTYPES;
    countProp.Flags = KSPROPERTY_TYPE_GET;
    if (!Control(engine->hFilter, IOCTL_KS_PROPERTY, &countProp, sizeof(countProp), &count, sizeof(count)))
        return NULL;
    PinRequest request = {0};
    request.connect.Interface.Set = GUID_KSINTERFACESETID_Standard;
    request.connect.Interface.Id = KSINTERFACE_STANDARD_STREAMING;
    request.connect.Medium.Set = GUID_KSMEDIUMSETID_Standard;
    request.connect.Medium.Id = KSMEDIUM_STANDARD_DEVIO;
    request.connect.Priority.PriorityClass = KSPRIORITY_NORMAL;
    request.connect.Priority.PrioritySubClass = 1;
    request.format.DataFormat.FormatSize = sizeof(request.format);
    request.format.DataFormat.MajorFormat = GUID_KSDATAFORMAT_TYPE_AUDIO;
    request.format.DataFormat.SubFormat = GUID_KSDATAFORMAT_SUBTYPE_PCM;
    request.format.DataFormat.Specifier = GUID_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    request.format.DataFormat.SampleSize = engine->channels * (engine->bitsPerSample / 8);
    WAVEFORMATEX* wf = &request.format.WaveFormatEx;
    wf->wFormatTag = WAVE_FORMAT_PCM;
    wf->nChannels = (WORD)engine->channels;
    wf->nSamplesPerSec = engine->sampleRate;
    wf->wBitsPerSample = (WORD)engine->bitsPerSample;
    wf->nBlockAlign = (WORD)request.format.DataFormat.SampleSize;
    wf->nAvgBytesPerSec = wf->nSamplesPerSec * wf->nBlockAlign;
    DWORD error = ERROR_NOT_SUPPORTED;
    for (ULONG id = 0; id < count; ++id) {
        KSPIN_DATAFLOW flow;
        KSPIN_COMMUNICATION communication;
        if (!PinProperty(engine->hFilter, id, KSPROPERTY_PIN_DATAFLOW, &flow, sizeof(flow)) || flow != direction)
            continue;
        if (!PinProperty(engine->hFilter, id, KSPROPERTY_PIN_COMMUNICATION, &communication, sizeof(communication)) ||
            (communication != KSPIN_COMMUNICATION_SINK && communication != KSPIN_COMMUNICATION_BOTH))
            continue;
        request.connect.PinId = id;
        HANDLE pin = NULL;
        // KsCreatePin validates this exact PCM format against the pin's data ranges.
        error = createPin(engine->hFilter, &request.connect,
                          direction == KSPIN_DATAFLOW_IN ? GENERIC_WRITE : GENERIC_READ, &pin);
        if (error == ERROR_SUCCESS && ValidHandle(pin)) return pin;
    }
    // KsCreatePin reports a pin held by another application as a resource
    // shortage, and a rejected format as a failed lookup.
    if (error == ERROR_NO_SYSTEM_RESOURCES || error == ERROR_BUSY || error == ERROR_SHARING_VIOLATION)
        error = KS_ERROR_DEVICE_IN_USE;
    else if (error == ERROR_NO_MATCH) error = KS_ERROR_FORMAT_UNSUPPORTED;
    SetLastError(error ? error : ERROR_NOT_SUPPORTED);
    return NULL;
}

static BOOL IsTargetDevice(const WCHAR* path) {
    // The UMC22's PCM2902 codec IDs are shared by other products. Never fall
    // back to an unrelated device or pair pins from separate audio filters.
    WCHAR lower[MAX_PATH * 4];
    size_t length = wcslen(path);
    if (length >= sizeof(lower) / sizeof(lower[0])) return FALSE;
    memcpy(lower, path, (length + 1) * sizeof(WCHAR));
    CharLowerBuffW(lower, (DWORD)length);
    const WCHAR* match = wcsstr(lower, L"vid_08bb&pid_2902");
    if (!match) return FALSE;
    WCHAR next = match[wcslen(L"vid_08bb&pid_2902")];
    return next == L'&' || next == L'#' || next == L'\\' || next == 0;
}

static BOOL ContainsNoCase(const WCHAR* text, const WCHAR* needle) {
    WCHAR lowerText[MAX_PATH * 4], lowerNeedle[KS_DEVICE_NAME_CHARS];
    size_t textLength = wcslen(text), needleLength = wcslen(needle);
    if (!needleLength || textLength >= sizeof(lowerText) / sizeof(lowerText[0]) ||
        needleLength >= sizeof(lowerNeedle) / sizeof(lowerNeedle[0])) return FALSE;
    memcpy(lowerText, text, (textLength + 1) * sizeof(WCHAR));
    memcpy(lowerNeedle, needle, (needleLength + 1) * sizeof(WCHAR));
    CharLowerBuffW(lowerText, (DWORD)textLength);
    CharLowerBuffW(lowerNeedle, (DWORD)needleLength);
    return wcsstr(lowerText, lowerNeedle) != NULL;
}

// A configured name replaces the hardware-ID filter. It may be part of the
// Device Manager name or, to tell identical units apart, of the instance ID.
static BOOL IsSelectedDevice(const WCHAR* name, const WCHAR* path, const WCHAR* label, const WCHAR* instanceId) {
    if (!name || !*name) return IsTargetDevice(path);
    return ContainsNoCase(label, name) || ContainsNoCase(instanceId, name) || ContainsNoCase(path, name);
}

static void DeviceLabel(HDEVINFO devices, SP_DEVINFO_DATA* info, WCHAR* label, DWORD chars) {
    label[0] = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(devices, info, SPDRP_FRIENDLYNAME, NULL,
                                           (BYTE*)label, chars * sizeof(WCHAR), NULL) &&
        !SetupDiGetDeviceRegistryPropertyW(devices, info, SPDRP_DEVICEDESC, NULL,
                                           (BYTE*)label, chars * sizeof(WCHAR), NULL))
        label[0] = 0;
    label[chars - 1] = 0;
}

static BOOL OpenAudioDevice(KSEngine* engine, CreatePinFn createPin, const WCHAR* deviceName) {
    HDEVINFO devices = SetupDiGetClassDevsW(&GUID_KSCATEGORY_RENDER, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return FALSE;
    SP_DEVICE_INTERFACE_DATA iface = {0};
    iface.cbSize = sizeof(iface);
    DWORD error = KS_ERROR_DEVICE_NOT_FOUND;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devices, NULL, &GUID_KSCATEGORY_RENDER, i, &iface); ++i) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &iface, NULL, 0, &size, NULL);
        if (!size) continue;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = malloc(size);
        if (!detail) { error = ERROR_NOT_ENOUGH_MEMORY; break; }
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA info = {0};
        info.cbSize = sizeof(info);
        WCHAR label[KS_DEVICE_NAME_CHARS], instanceId[MAX_PATH] = {0};
        BOOL target = SetupDiGetDeviceInterfaceDetailW(devices, &iface, detail, size, NULL, &info);
        if (target) {
            DeviceLabel(devices, &info, label, KS_DEVICE_NAME_CHARS);
            if (!SetupDiGetDeviceInstanceIdW(devices, &info, instanceId, MAX_PATH, NULL)) instanceId[0] = 0;
            target = IsSelectedDevice(deviceName, detail->DevicePath, label, instanceId);
        }
        if (target) {
            engine->hFilter = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
            if (ValidHandle(engine->hFilter)) {
                engine->hRenderPin = CreateAudioPin(engine, createPin, KSPIN_DATAFLOW_IN);
                if (engine->hRenderPin)
                    engine->hCapturePin = CreateAudioPin(engine, createPin, KSPIN_DATAFLOW_OUT);
                if (engine->hRenderPin && engine->hCapturePin) {
                    wcsncpy(engine->deviceName, label[0] ? label : instanceId, KS_DEVICE_NAME_CHARS - 1);
                    if (!SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_SERVICE, NULL, (BYTE*)engine->deviceService,
                                                           sizeof(engine->deviceService) - sizeof(WCHAR), NULL))
                        engine->deviceService[0] = 0;
                    // Baseline estimate for Windows' default USB capture buffering.
                    // A duration request is not a measurement; loopback offsets
                    // account for the actual delay.
                    if (_wcsicmp(engine->deviceService, L"usbaudio") == 0) engine->inputDelaySamples = engine->sampleRate / 100;
                    free(detail);
                    SetupDiDestroyDeviceInfoList(devices);
                    return TRUE;
                }
                error = GetLastError();
                if (engine->hRenderPin) CloseHandle(engine->hRenderPin);
                CloseHandle(engine->hFilter);
                engine->hRenderPin = NULL;
            } else error = GetLastError();
            engine->hFilter = NULL;
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(devices);
    SetLastError(error);
    return FALSE;
}

static void RecordError(KSEngine* engine, DWORD error) {
    if (!error) error = ERROR_GEN_FAILURE;
    InterlockedCompareExchange(&engine->lastError, (LONG)error, ERROR_SUCCESS);
    InterlockedIncrement64(&engine->ioErrors);
}

static BOOL StopRequested(KSEngine* engine) {
    return __atomic_load_n(&engine->stopRequested, __ATOMIC_RELAXED);
}

void KSEngine_RequestStop(KSEngine* engine) {
    __atomic_store_n(&engine->stopRequested, TRUE, __ATOMIC_RELAXED);
    if (engine->hStopEvent) SetEvent(engine->hStopEvent);
}

static BOOL SubmitPacket(KSEngine* engine, HANDLE pin, KSPacket* packet, BOOL capture) {
    if (packet->pending) { RecordError(engine, ERROR_BUSY); return FALSE; }
    ResetEvent(packet->event);
    ZeroMemory(&packet->overlapped, sizeof(packet->overlapped));
    packet->overlapped.hEvent = packet->event;
    ZeroMemory(&packet->header, sizeof(packet->header));
    packet->header.Size = sizeof(packet->header);
    packet->header.Data = packet->data;
    packet->header.FrameExtent = engine->bufferSizeBytes;
    packet->header.DataUsed = capture ? 0 : engine->bufferSizeBytes;
    // No TIMEVALID flag: these packets do not carry presentation timestamps.
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(pin, capture ? IOCTL_KS_READ_STREAM : IOCTL_KS_WRITE_STREAM,
        &packet->header, sizeof(packet->header), &packet->header, sizeof(packet->header), &bytes, &packet->overlapped);
    if (ok) return TRUE;
    DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) { packet->pending = TRUE; return TRUE; }
    RecordError(engine, error);
    return FALSE;
}

static BOOL CompletePacket(KSEngine* engine, HANDLE pin, KSPacket* packet) {
    if (!packet->pending) return TRUE;
    DWORD bytes = 0;
    BOOL ok = GetOverlappedResult(pin, &packet->overlapped, &bytes, FALSE);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (error != ERROR_IO_INCOMPLETE) packet->pending = FALSE;
    if (!ok) { RecordError(engine, error); return FALSE; }
    return TRUE;
}

static BOOL WaitPacket(KSEngine* engine, HANDLE pin, KSPacket* packet) {
    if (StopRequested(engine)) return FALSE;
    if (!packet->pending) return TRUE;
    HANDLE handles[] = {engine->hStopEvent, packet->event};
    DWORD result = WaitForMultipleObjects(2, handles, FALSE, 500);
    if (result == WAIT_OBJECT_0) return FALSE;
    if (result != WAIT_OBJECT_0 + 1) {
        if (result == WAIT_TIMEOUT) InterlockedIncrement64(&engine->waitTimeouts);
        RecordError(engine, result == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
        return FALSE;
    }
    return CompletePacket(engine, pin, packet);
}

static void DrainPackets(KSEngine* engine) {
    if (ValidHandle(engine->hRenderPin)) CancelIoEx(engine->hRenderPin, NULL);
    if (ValidHandle(engine->hCapturePin)) CancelIoEx(engine->hCapturePin, NULL);
    // STOP also releases queued KS requests. Never use a timeout as permission
    // to free storage still owned by a worker or outstanding I/O request.
    if (!SetPinState(engine->hRenderPin, KSSTATE_STOP)) RecordError(engine, GetLastError());
    if (!SetPinState(engine->hCapturePin, KSSTATE_STOP)) RecordError(engine, GetLastError());
    for (ULONG p = 0; p < KS_ALL_PACKETS; ++p) {
        KSPacket* packet = Packet(engine, p);
        if (!packet->pending) continue;
        DWORD bytes = 0;
        HANDLE pin = p < KS_MAX_RENDER_PACKETS ? engine->hRenderPin : engine->hCapturePin;
        if (!GetOverlappedResult(pin, &packet->overlapped, &bytes, TRUE)) {
            DWORD error = GetLastError();
            if (error != ERROR_OPERATION_ABORTED) RecordError(engine, error);
        }
        packet->pending = FALSE;
    }
}

static void StoreMaximum(volatile LONG64* target, LONG64 value) {
    LONG64 previous = InterlockedCompareExchange64(target, 0, 0);
    while (value > previous) {
        LONG64 observed = InterlockedCompareExchange64(target, value, previous);
        if (observed == previous) break;
        previous = observed;
    }
}

static DWORD WINAPI AudioStreamingThread(void* context) {
    KSEngine* engine = context;
    // Publish the thread handle/id before any host callback can request stop.
    WaitForSingleObject(engine->hLaunchEvent, INFINITE);
    HMODULE avrt = LoadLibraryW(L"avrt.dll");
    HANDLE task = NULL;
    RevertCharacteristicsFn revert = NULL;
    if (avrt) {
        SetCharacteristicsFn characteristics = (SetCharacteristicsFn)(void*)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsA");
        SetPriorityFn priority = (SetPriorityFn)(void*)GetProcAddress(avrt, "AvSetMmThreadPriority");
        revert = (RevertCharacteristicsFn)(void*)GetProcAddress(avrt, "AvRevertMmThreadCharacteristics");
        DWORD index = 0;
        if (characteristics && revert) task = characteristics("Pro Audio", &index);
        if (task && priority) priority(task, 2);
    }
    if (!task) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    if (!SetBothStates(engine, KSSTATE_ACQUIRE) || !SetBothStates(engine, KSSTATE_PAUSE)) {
        RecordError(engine, GetLastError());
        goto finish;
    }
    const ULONG renderPackets = engine->renderPackets, capturePackets = engine->capturePackets;
    for (ULONG p = 0; p < capturePackets; ++p)
        if (!SubmitPacket(engine, engine->hCapturePin, &engine->capture[p], TRUE)) goto finish;
    for (ULONG p = 0; p < renderPackets; ++p) {
        memset(engine->render[p].data, 0, engine->bufferSizeBytes);
        if (!SubmitPacket(engine, engine->hRenderPin, &engine->render[p], FALSE)) goto finish;
    }
    if (StopRequested(engine)) goto finish;
    if (!SetBothStates(engine, KSSTATE_RUN)) { RecordError(engine, GetLastError()); goto finish; }
    InterlockedExchange(&engine->isRunning, TRUE);
    SetEvent(engine->hReadyEvent);
    LARGE_INTEGER previous = {0};
    const LONG64 period = engine->counterFrequency.QuadPart * engine->bufferSizeSamples / engine->sampleRate;
    const LONG64 margin = engine->counterFrequency.QuadPart / 2000; // 0.5 ms of timing noise.
    LONG64 recoveryBudget = engine->counterFrequency.QuadPart / 10000; // Stop starting skips after 100 us.
    if (recoveryBudget > period / 8) recoveryBudget = period / 8;
    if (recoveryBudget < 1) recoveryBudget = 1;
    // Input alignment: input that stayed queued beyond a reserve of at least 2 ms
    // (and one buffer) for a whole 40 ms window always arrived well before it was
    // needed, so it only adds latency; trim it. With Windows' default 10 ms capture
    // blocks the minimum is near zero and nothing is trimmed. A dropout soon after
    // a trim keeps one more buffer in reserve from then on.
    const LONG64 window = engine->counterFrequency.QuadPart / 25;
    ULONG reserve = (engine->sampleRate / 500 + engine->bufferSizeSamples - 1) / engine->bufferSizeSamples;
    if (reserve < 1) reserve = 1;
    InterlockedExchange(&engine->inputReserve, (LONG)reserve);
    ULONG trims = 0, minAhead = 0, current = 0, captureIndex = 0;
    LONG64 due = 0, excess = 0, excessSince = 0, windowStart = 0, lastTrim = 0;
    long asioIndex = 0; // The host's double buffer alternates whatever the queue depth.
    int settle = 0;
    while (!StopRequested(engine)) {
        KSPacket* render = &engine->render[current];
        KSPacket* capture = &engine->capture[captureIndex];
        if (!WaitPacket(engine, engine->hRenderPin, render) ||
            !WaitPacket(engine, engine->hCapturePin, capture) || StopRequested(engine)) break;
        // Output is queued up to the most recently submitted packet; once that one
        // has finished playing, output has run dry.
        KSPacket* newest = &engine->render[(current + renderPackets - 1) % renderPackets];
        // Skip already-recorded input that is not needed: input recorded while output
        // was stalled, and input queued beyond the alignment reserve. Never wait for
        // input here, which would starve output.
        LARGE_INTEGER recoveryStart = {0};
        if (excess >= period + margin || trims) QueryPerformanceCounter(&recoveryStart);
        for (int skipped = 0; (excess >= period + margin || trims) && skipped < KS_MAX_RECOVERY_SKIPS; ++skipped) {
            KSPacket* next = &engine->capture[(captureIndex + 1) % capturePackets];
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (next->pending && !HasOverlappedIoCompleted(&next->overlapped)) {
                // No more recorded input is queued. A trim saw it queued for a whole
                // window, so it is gone: cancel the trim. Input owed after a dropout may
                // still arrive with the next capture block, unless none has been
                // skippable for two windows: then both directions stalled together.
                trims = 0;
                if (now.QuadPart - excessSince > 2 * window) excess = 0;
                break;
            }
            if (StopRequested(engine) || now.QuadPart - recoveryStart.QuadPart >= recoveryBudget ||
                (newest->pending && HasOverlappedIoCompleted(&newest->overlapped))) break;
            if (!SubmitPacket(engine, engine->hCapturePin, capture, TRUE) ||
                !CompletePacket(engine, engine->hCapturePin, next)) goto finish;
            if (excess >= period + margin) {
                excess -= period;
                excessSince = now.QuadPart; // Progress: keep the rest owed.
                InterlockedIncrement64(&engine->inputSkips);
            } else {
                --trims;
                lastTrim = now.QuadPart;
                InterlockedIncrement64(&engine->inputTrims);
            }
            captureIndex = (captureIndex + 1) % capturePackets;
            capture = next;
        }
        if (StopRequested(engine)) break;
        ULONG used = capture->header.DataUsed;
        if (used > engine->bufferSizeBytes || used % (engine->channels * (engine->bitsPerSample / 8))) {
            RecordError(engine, ERROR_INVALID_DATA);
            break;
        }
        if (used < engine->bufferSizeBytes) {
            memset(capture->data + used, 0, engine->bufferSizeBytes - used);
            InterlockedIncrement64(&engine->shortReads);
        }
        LARGE_INTEGER before, after;
        QueryPerformanceCounter(&before);
        if (previous.QuadPart) StoreMaximum(&engine->maxCallbackGapTicks, before.QuadPart - previous.QuadPart);
        previous = before;
        // Callbacks follow the device clock. After a dropout, output plays later by
        // as long as its queue stayed empty, while input kept recording. Once output
        // is steady again, that delay shows up here as lateness.
        if (!due) due = before.QuadPart;
        LONG64 lateness = before.QuadPart - due;
        if (settle) {
            if (!--settle) {
                if (lateness > 0) {
                    excess += lateness;
                    excessSince = before.QuadPart;
                    InterlockedExchangeAdd64(&engine->slipSamples,
                                             lateness * engine->sampleRate / engine->counterFrequency.QuadPart);
                }
                due = before.QuadPart;
            }
        } else if (lateness > -period && lateness < period) due += lateness / 64; // Follow clock drift.
        due += period;
        // Recorded packets queued behind this one; capture requests complete in order.
        ULONG ahead = 0;
        while (ahead + 1 < capturePackets) {
            KSPacket* queued = &engine->capture[(captureIndex + 1 + ahead) % capturePackets];
            if (queued->pending && !HasOverlappedIoCompleted(&queued->overlapped)) break;
            ++ahead;
        }
        if (settle || excess >= period + margin || trims) windowStart = 0; // Still realigning.
        else if (!windowStart) {
            windowStart = before.QuadPart;
            minAhead = ahead;
        } else {
            if (ahead < minAhead) minAhead = ahead;
            if (before.QuadPart - windowStart >= window) {
                InterlockedExchange(&engine->queuedInputSamples, (LONG)(minAhead * engine->bufferSizeSamples));
                if (minAhead > reserve) trims = minAhead - reserve;
                windowStart = 0;
            }
        }
        ULONGLONG seconds = (ULONGLONG)(before.QuadPart / engine->counterFrequency.QuadPart);
        ULONGLONG ns = seconds * 1000000000ULL + (ULONGLONG)(
            (double)(before.QuadPart % engine->counterFrequency.QuadPart) * 1e9 / engine->counterFrequency.QuadPart);
        engine->onInputReady(engine->callbackContext, asioIndex, capture->data);
        // The host now owns its converted input. Keep all capture requests in
        // flight while its DSP runs; it must no longer read capture->data.
        if (StopRequested(engine) || !SubmitPacket(engine, engine->hCapturePin, capture, TRUE)) break;
        engine->onBufferReady(engine->callbackContext, asioIndex, render->data, ns);
        QueryPerformanceCounter(&after);
        LONG64 elapsed = after.QuadPart - before.QuadPart;
        StoreMaximum(&engine->maxCallbackTicks, elapsed);
        InterlockedIncrement64(&engine->callbacks);
        if (elapsed > period) InterlockedIncrement64(&engine->callbackOverruns);
        if (StopRequested(engine)) break;
        // If the newest queued output packet already finished, output ran out of
        // audio before this buffer: a dropout.
        if (newest->pending && HasOverlappedIoCompleted(&newest->overlapped)) {
            InterlockedIncrement64(&engine->dropouts);
            settle = (int)renderPackets; // Measure once the queue has refilled behind this buffer.
            trims = 0;
            if (lastTrim && before.QuadPart - lastTrim < 2 * window && reserve + 1 < capturePackets)
                InterlockedExchange(&engine->inputReserve, (LONG)++reserve);
        }
        if (!SubmitPacket(engine, engine->hRenderPin, render, FALSE)) break;
        current = (current + 1) % renderPackets;
        captureIndex = (captureIndex + 1) % capturePackets;
        asioIndex = 1 - asioIndex;
    }
finish:
    DrainPackets(engine);
    InterlockedExchange(&engine->isRunning, FALSE);
    SetEvent(engine->hReadyEvent);
    if (task) revert(task);
    if (avrt) FreeLibrary(avrt);
    DWORD error = (DWORD)InterlockedCompareExchange(&engine->lastError, 0, 0);
    if (error && engine->onError && !StopRequested(engine)) engine->onError(engine->callbackContext, error);
    return error;
}

BOOL KSEngine_Init(KSEngine* engine, ULONG rate, ULONG channels, ULONG bits, ULONG samples,
                   const WCHAR* deviceName) {
    ZeroMemory(engine, sizeof(*engine));
    if ((rate != 44100 && rate != 48000) || channels != 2 || bits != 16 ||
        samples < KS_MIN_BUFFER_SAMPLES || samples > KS_MAX_BUFFER_SAMPLES || samples % KS_BUFFER_GRANULARITY) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    engine->sampleRate = rate;
    engine->channels = channels;
    engine->bitsPerSample = bits;
    engine->bufferSizeSamples = samples;
    engine->bufferSizeBytes = samples * channels * (bits / 8);
    if (!QueryPerformanceFrequency(&engine->counterFrequency)) goto failure;
    engine->hKsUser = LoadLibraryW(L"ksuser.dll");
    if (!engine->hKsUser) goto failure;
    CreatePinFn createPin = (CreatePinFn)(void*)GetProcAddress(engine->hKsUser, "KsCreatePin");
    if (!createPin || !OpenAudioDevice(engine, createPin, deviceName)) goto failure;
    engine->hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!engine->hStopEvent) goto failure;
    engine->hReadyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!engine->hReadyEvent) goto failure;
    engine->hLaunchEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!engine->hLaunchEvent) goto failure;
    // Every render packet is allocated, so extra output buffers need no allocation later.
    engine->renderPackets = KS_MIN_PACKETS;
    engine->capturePackets = CapturePacketsFor(samples, rate);
    for (ULONG p = 0; p < KS_MAX_RENDER_PACKETS + engine->capturePackets; ++p) {
        KSPacket* packet = Packet(engine, p);
        packet->data = VirtualAlloc(NULL, engine->bufferSizeBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!packet->data) goto failure;
        memset(packet->data, 0, engine->bufferSizeBytes);
        packet->locked = VirtualLock(packet->data, engine->bufferSizeBytes);
        if (!packet->locked) InterlockedIncrement(&engine->pageLockFailures);
        packet->event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!packet->event) goto failure;
    }
    return TRUE;
failure: {
    DWORD error = GetLastError();
    KSEngine_Cleanup(engine);
    SetLastError(error ? error : ERROR_GEN_FAILURE);
    return FALSE;
}}

BOOL KSEngine_SetCaptureBufferDuration(KSEngine* engine, ULONG milliseconds) {
    if ((milliseconds && (milliseconds < 3 || milliseconds > 10)) || engine->hThread ||
        !ValidHandle(engine->hCapturePin)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!milliseconds) return TRUE;
    engine->captureBufferMs = milliseconds;
    engine->captureBufferError = ERROR_SUCCESS;
    KSPROPERTY prop = {0};
    prop.Set = GUID_KSPROPSETID_AudioBufferDuration;
    prop.Id = KSPROPERTY_AUDIO_BUFFER_DURATION;
    prop.Flags = KSPROPERTY_TYPE_SET;
    BOOL ok = Control(engine->hCapturePin, IOCTL_KS_PROPERTY, &prop, sizeof(prop),
                      &milliseconds, sizeof(milliseconds));
    if (!ok) engine->captureBufferError = GetLastError();
    // SET success is a hint acknowledgment, not a measurement. Keep the
    // conservative latency estimate until the user calibrates with loopback.
    return ok;
}

BOOL KSEngine_SetExtraOutputBuffers(KSEngine* engine, ULONG extra) {
    if (extra > KS_MAX_EXTRA_OUTPUT_BUFFERS || engine->hThread || !engine->render[KS_MAX_RENDER_PACKETS - 1].data) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    engine->renderPackets = KS_MIN_PACKETS + extra;
    return TRUE;
}

BOOL KSEngine_Start(KSEngine* engine, void (*inputReady)(void*, long, const void*),
                   void (*callback)(void*, long, void*, ULONGLONG),
                   void (*onError)(void*, DWORD), void* context) {
    if (!ValidHandle(engine->hRenderPin) || !ValidHandle(engine->hCapturePin) ||
        !inputReady || !callback || engine->hThread) {
        SetLastError(ERROR_INVALID_STATE);
        return FALSE;
    }
    engine->onInputReady = inputReady;
    engine->onBufferReady = callback;
    engine->onError = onError;
    engine->callbackContext = context;
    InterlockedExchange(&engine->lastError, 0);
    InterlockedExchange64(&engine->callbacks, 0);
    InterlockedExchange64(&engine->waitTimeouts, 0);
    InterlockedExchange64(&engine->ioErrors, 0);
    InterlockedExchange64(&engine->shortReads, 0);
    InterlockedExchange64(&engine->callbackOverruns, 0);
    InterlockedExchange64(&engine->maxCallbackTicks, 0);
    InterlockedExchange64(&engine->maxCallbackGapTicks, 0);
    InterlockedExchange64(&engine->dropouts, 0);
    InterlockedExchange64(&engine->inputSkips, 0);
    InterlockedExchange64(&engine->slipSamples, 0);
    InterlockedExchange64(&engine->inputTrims, 0);
    InterlockedExchange(&engine->queuedInputSamples, -1);
    InterlockedExchange(&engine->inputReserve, 0);
    ResetEvent(engine->hStopEvent);
    __atomic_store_n(&engine->stopRequested, FALSE, __ATOMIC_RELAXED);
    ResetEvent(engine->hReadyEvent);
    ResetEvent(engine->hLaunchEvent);
    engine->hThread = CreateThread(NULL, 0, AudioStreamingThread, engine, 0, &engine->threadId);
    if (!engine->hThread) return FALSE;
    SetEvent(engine->hLaunchEvent);
    HANDLE handles[] = {engine->hReadyEvent, engine->hThread};
    DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    DWORD error = (DWORD)InterlockedCompareExchange(&engine->lastError, 0, 0);
    if (result == WAIT_OBJECT_0 && !error && InterlockedCompareExchange(&engine->isRunning, 0, 0)) return TRUE;
    if (!error) error = result == WAIT_FAILED ? GetLastError() : ERROR_OPERATION_ABORTED;
    KSEngine_Stop(engine);
    SetLastError(error);
    return FALSE;
}

BOOL KSEngine_Stop(KSEngine* engine) {
    KSEngine_RequestStop(engine);
    if (engine->hThread) {
        if (GetCurrentThreadId() == engine->threadId) { SetLastError(ERROR_BUSY); return FALSE; }
        if (WaitForSingleObject(engine->hThread, INFINITE) != WAIT_OBJECT_0) return FALSE;
        CloseHandle(engine->hThread);
        engine->hThread = NULL;
        engine->threadId = 0;
    }
    InterlockedExchange(&engine->isRunning, FALSE);
    return TRUE;
}

BOOL KSEngine_Cleanup(KSEngine* engine) {
    if (!KSEngine_Stop(engine)) return FALSE;
    for (ULONG p = 0; p < KS_ALL_PACKETS; ++p) {
        KSPacket* packet = Packet(engine, p);
        if (packet->locked) VirtualUnlock(packet->data, engine->bufferSizeBytes);
        if (packet->data) VirtualFree(packet->data, 0, MEM_RELEASE);
        if (packet->event) CloseHandle(packet->event);
    }
    if (ValidHandle(engine->hRenderPin)) CloseHandle(engine->hRenderPin);
    if (ValidHandle(engine->hCapturePin)) CloseHandle(engine->hCapturePin);
    if (ValidHandle(engine->hFilter)) CloseHandle(engine->hFilter);
    if (engine->hStopEvent) CloseHandle(engine->hStopEvent);
    if (engine->hReadyEvent) CloseHandle(engine->hReadyEvent);
    if (engine->hLaunchEvent) CloseHandle(engine->hLaunchEvent);
    if (engine->hKsUser) FreeLibrary(engine->hKsUser);
    ZeroMemory(engine, sizeof(*engine));
    return TRUE;
}

void KSEngine_GetStats(KSEngine* engine, KSStats* stats) {
    stats->callbacks = InterlockedCompareExchange64(&engine->callbacks, 0, 0);
    stats->waitTimeouts = InterlockedCompareExchange64(&engine->waitTimeouts, 0, 0);
    stats->ioErrors = InterlockedCompareExchange64(&engine->ioErrors, 0, 0);
    stats->shortReads = InterlockedCompareExchange64(&engine->shortReads, 0, 0);
    stats->callbackOverruns = InterlockedCompareExchange64(&engine->callbackOverruns, 0, 0);
    stats->maxCallbackTicks = InterlockedCompareExchange64(&engine->maxCallbackTicks, 0, 0);
    stats->maxCallbackGapTicks = InterlockedCompareExchange64(&engine->maxCallbackGapTicks, 0, 0);
    stats->counterFrequency = engine->counterFrequency.QuadPart;
    stats->dropouts = InterlockedCompareExchange64(&engine->dropouts, 0, 0);
    stats->inputSkips = InterlockedCompareExchange64(&engine->inputSkips, 0, 0);
    stats->slipSamples = InterlockedCompareExchange64(&engine->slipSamples, 0, 0);
    stats->inputTrims = InterlockedCompareExchange64(&engine->inputTrims, 0, 0);
    stats->queuedInputSamples = InterlockedCompareExchange(&engine->queuedInputSamples, 0, 0);
    stats->inputReserve = InterlockedCompareExchange(&engine->inputReserve, 0, 0);
    stats->pageLockFailures = InterlockedCompareExchange(&engine->pageLockFailures, 0, 0);
    stats->lastError = (DWORD)InterlockedCompareExchange(&engine->lastError, 0, 0);
}
