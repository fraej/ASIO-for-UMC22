#include "../asio_types.h"
#include "../ks_engine.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>

static bool failAllocation, failLock, failInit, failStart, failStop;
static int allocations, frees, unlocks, initCalls, legacyCalls, timeCalls, resetCalls, overloadCalls, messageBoxes;
static bool timeSupported, resetSupported, overloadSupported, profileMissing;
static KSEngine* lastEngine;
static DWORD initError = ERROR_NOT_FOUND;
static ULONG engineInputDelay;
static UINT profileRate = 48000, profileBuffer = 96;
static const WCHAR* profileDeviceName = L"";
static const WCHAR* profileInputOffset = L"";
static const WCHAR* profileOutputOffset = L"";
static const WCHAR* profileCaptureBuffer = L"";
static const WCHAR* profileExtra = L"";
static ULONG lastExtraSet = 99;
static DWORD captureHintError;
static char panelText[3072];
static WCHAR lastDeviceName[KS_DEVICE_NAME_CHARS];
static LPVOID WINAPI TestAlloc(LPVOID a, SIZE_T b, DWORD c, DWORD d) {
    if (failAllocation) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    LPVOID result = VirtualAlloc(a, b, c, d);
    if (result) ++allocations;
    return result;
}
static BOOL WINAPI TestFree(LPVOID a, SIZE_T b, DWORD c) { ++frees; return VirtualFree(a, b, c); }
static BOOL WINAPI TestLock(LPVOID, SIZE_T) { return !failLock; }
static BOOL WINAPI TestUnlock(LPVOID, SIZE_T) { ++unlocks; return TRUE; }
static UINT WINAPI TestProfile(LPCWSTR, LPCWSTR key, INT fallback, LPCWSTR) {
    if (profileMissing) return (UINT)fallback;
    return wcscmp(key, L"SampleRate") == 0 ? profileRate : profileBuffer;
}
static DWORD WINAPI TestProfileString(LPCWSTR, LPCWSTR key, LPCWSTR fallback, LPWSTR out, DWORD size, LPCWSTR) {
    const WCHAR* value = fallback;
    if (!profileMissing && wcscmp(key, L"DeviceName") == 0) value = profileDeviceName;
    if (!profileMissing && wcscmp(key, L"InputLatencyOffset") == 0) value = profileInputOffset;
    if (!profileMissing && wcscmp(key, L"OutputLatencyOffset") == 0) value = profileOutputOffset;
    if (!profileMissing && wcscmp(key, L"CaptureBufferMs") == 0) value = profileCaptureBuffer;
    if (!profileMissing && wcscmp(key, L"ExtraOutputBuffers") == 0) value = profileExtra;
    lstrcpynW(out, value, (int)size);
    return (DWORD)wcslen(out);
}
static LONG WINAPI TestRegCreate(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM,
                                const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD) { return ERROR_ACCESS_DENIED; }
static int WINAPI TestMessageBox(HWND, LPCSTR, LPCSTR, UINT) { ++messageBoxes; return IDOK; }
static int dialogs;
static long dialogChoice, dialogExtra = -1; // -1 leaves the output safety choice unchanged.
static INT_PTR dialogResult = IDCANCEL;
static WCHAR savedBufferSize[16], savedExtra[16];
static BOOL WINAPI TestWriteProfile(LPCWSTR section, LPCWSTR key, LPCWSTR value, LPCWSTR) {
    assert(wcscmp(section, L"Audio") == 0);
    if (wcscmp(key, L"BufferSize") == 0) lstrcpynW(savedBufferSize, value, 16);
    else if (wcscmp(key, L"ExtraOutputBuffers") == 0) lstrcpynW(savedExtra, value, 16);
    else assert(!"unexpected setting");
    return TRUE;
}

static INT_PTR WINAPI TestDialog(HINSTANCE, LPCDLGTEMPLATEW, HWND, DLGPROC, LPARAM context);
#define VirtualAlloc TestAlloc
#define VirtualFree TestFree
#define VirtualLock TestLock
#define VirtualUnlock TestUnlock
#define GetPrivateProfileIntW TestProfile
#define GetPrivateProfileStringW TestProfileString
#define RegCreateKeyExW TestRegCreate
#define MessageBoxA TestMessageBox
#define WritePrivateProfileStringW TestWriteProfile
#define DialogBoxIndirectParamW TestDialog
#include "../asio_driver.cpp"

// Stands in for the user: checks what the panel offers, then picks dialogChoice.
static INT_PTR WINAPI TestDialog(HINSTANCE, LPCDLGTEMPLATEW dialog, HWND, DLGPROC proc, LPARAM lParam) {
    panel::Context* context = reinterpret_cast<panel::Context*>(lParam);
    assert(dialog && dialog->cdit > 20 && proc == panel::Proc && context->text && strstr(context->text, "Output dropouts"));
    assert(context->rowCount >= 18 && context->summary[0]);
    assert(context->extraOutputBuffers >= 0 && context->extraOutputBuffers <= KS_MAX_EXTRA_OUTPUT_BUFFERS);
    lstrcpynA(panelText, context->text, sizeof(panelText));
    ++dialogs;
    if (dialogResult == IDOK) context->bufferSize = dialogChoice;
    if (dialogResult == IDOK && dialogExtra >= 0) context->extraOutputBuffers = dialogExtra;
    return dialogResult;
}

extern "C" BOOL KSEngine_Init(KSEngine* e, ULONG rate, ULONG channels, ULONG bits, ULONG samples,
                              const WCHAR* deviceName) {
    ++initCalls;
    lstrcpynW(lastDeviceName, deviceName ? deviceName : L"", KS_DEVICE_NAME_CHARS);
    ZeroMemory(e, sizeof(*e));
    if (failInit) { SetLastError(initError); return FALSE; }
    e->sampleRate = rate;
    e->channels = channels;
    e->bitsPerSample = bits;
    e->bufferSizeSamples = samples;
    e->inputDelaySamples = engineInputDelay;
    e->hCapturePin = (HANDLE)(uintptr_t)0x2000;
    e->renderPackets = KS_MIN_PACKETS;
    lastEngine = e;
    return TRUE;
}
extern "C" BOOL KSEngine_SetExtraOutputBuffers(KSEngine* e, ULONG extra) {
    lastExtraSet = extra;
    if (extra > KS_MAX_EXTRA_OUTPUT_BUFFERS) return FALSE;
    e->renderPackets = KS_MIN_PACKETS + extra;
    return TRUE;
}
extern "C" BOOL KSEngine_SetCaptureBufferDuration(KSEngine* e, ULONG ms) {
    e->captureBufferMs = ms;
    e->captureBufferError = ms ? captureHintError : ERROR_SUCCESS;
    return e->captureBufferError == ERROR_SUCCESS;
}
extern "C" BOOL KSEngine_Start(KSEngine* e, void (*input)(void*, long, const void*),
                              void (*cb)(void*, long, void*, ULONGLONG),
                              void (*error)(void*, DWORD), void* context) {
    if (failStart) { SetLastError(ERROR_DEVICE_NOT_CONNECTED); return FALSE; }
    e->onBufferReady = cb;
    e->onInputReady = input;
    e->onError = error;
    e->callbackContext = context;
    InterlockedExchange(&e->isRunning, TRUE);
    return TRUE;
}
extern "C" BOOL KSEngine_Stop(KSEngine* e) {
    if (failStop) { SetLastError(ERROR_BUSY); return FALSE; }
    InterlockedExchange(&e->isRunning, FALSE);
    return TRUE;
}
extern "C" BOOL KSEngine_Cleanup(KSEngine* e) { ZeroMemory(e, sizeof(*e)); return TRUE; }
extern "C" void KSEngine_GetStats(KSEngine*, KSStats* s) { ZeroMemory(s, sizeof(*s)); }
static void Legacy(long, ASIOBool) { ++legacyCalls; }
static ASIOTime* TimeInfo(ASIOTime* time, long, ASIOBool) {
    ++timeCalls;
    assert(time->timeInfo.flags == (kSystemTimeValid | kSamplePositionValid | kSampleRateValid | kSpeedValid));
    return time;
}
static long Message(long selector, long value, void*, double*) {
    if (selector == kAsioSupportsTimeInfo) return timeSupported;
    if (selector == kAsioSelectorSupported && value == kAsioResetRequest) return resetSupported;
    if (selector == kAsioSelectorSupported && value == kAsioOverload) return overloadSupported;
    if (selector == kAsioResetRequest) ++resetCalls;
    if (selector == kAsioOverload) ++overloadCalls;
    if (selector == kAsioEngineVersion) return 2;
    return 0;
}
static ASIOCallbacks Callbacks() { return {Legacy, NULL, Message, TimeInfo}; }
static ASIOBufferInfo Buffer(long ch = 0, ASIOBool input = ASIOFalse) {
    ASIOBufferInfo b = {};
    b.channelNum = ch;
    b.isInput = input;
    return b;
}
static void DeliverBuffer(KSEngine* e, long index, const void* input, void* output, ULONGLONG timestamp) {
    e->onInputReady(e->callbackContext, index, input);
    e->onBufferReady(e->callbackContext, index, output, timestamp);
}

int main() {
    {
        MiniAsioDriver d;
        ASIOCallbacks cb = Callbacks();
        ASIOBufferInfo b = Buffer();
        assert(d.start() == ASE_InvalidMode);
        for (long size : {0L, -1L, 47L, 49L, 1024L, 0x7fffffffL})
            assert(d.createBuffers(&b, 1, size, &cb) == ASE_InvalidParameter);
        b.channelNum = -1;
        assert(d.createBuffers(&b, 1, 96, &cb) == ASE_InvalidParameter);
        b.channelNum = 2;
        assert(d.createBuffers(&b, 1, 96, &cb) == ASE_InvalidParameter);
        b = Buffer();
        ASIOBufferInfo duplicate[] = {b, b};
        assert(d.createBuffers(duplicate, 2, 96, &cb) == ASE_InvalidParameter);
        assert(initCalls == 0 && allocations == 0);
        failAllocation = true;
        assert(d.createBuffers(&b, 1, 96, &cb) == ASE_NoMemory);
        assert(!b.buffers[0] && !b.buffers[1]);
        failAllocation = false;
        failInit = true;
        assert(d.createBuffers(&b, 1, 96, &cb) == ASE_HWMalfunction);
        assert(allocations == frees && !b.buffers[0]);
        failInit = false;
        failLock = true;
        assert(d.createBuffers(&b, 1, 96, &cb) == ASE_OK);
        assert(d.createBuffers(&b, 1, 96, &cb) == ASE_InvalidMode);
        assert(d.setSampleRate(44100) == ASE_InvalidMode);
        failStart = true;
        assert(d.start() == ASE_HWMalfunction);
        failStart = false;
        assert(d.start() == ASE_OK);
        assert(d.setSampleRate(44100) == ASE_InvalidMode);
        ASIOSampleRate rate;
        assert(d.getSampleRate(&rate) == ASE_OK && rate == 48000 && lastEngine->sampleRate == 48000);
        failStop = true;
        int freed = frees;
        assert(d.disposeBuffers() == ASE_InvalidMode && frees == freed);
        failStop = false;
        assert(d.disposeBuffers() == ASE_OK);
        assert(unlocks == 1); // Only the earlier allocation (before lock failure) was locked.
        failLock = false;
        assert(d.setSampleRate(44100) == ASE_OK);
        assert(d.createBuffers(&b, 1, 144, &cb) == ASE_OK && lastEngine->sampleRate == 44100);
        assert(lastDeviceName[0] == 0); // No setting: engine uses the hardware-ID filter.
        assert(d.disposeBuffers() == ASE_OK && allocations == frees);
    }
    {
        MiniAsioDriver d;
        ASIOCallbacks cb = Callbacks();
        ASIOBufferInfo b = Buffer(1);
        timeSupported = false;
        assert(d.createBuffers(&b, 1, 96, &cb) == ASE_OK);
        cb.bufferSwitch = NULL; // Verify that the driver copied the callback table.
        for (int i = 0; i < 96; ++i) ((float*)b.buffers[0])[i] = 0.25f;
        int16_t output[192];
        assert(d.start() == ASE_OK);
        DeliverBuffer(lastEngine, 0, NULL, output, 123456789);
        assert(legacyCalls == 1 && timeCalls == 0);
        for (int i = 0; i < 96; ++i) assert(output[i * 2] == 0 && output[i * 2 + 1] == 8192);
        ASIOSamples position;
        ASIOTimeStamp stamp;
        assert(d.getSamplePosition(&position, &stamp) == ASE_OK && position.lo == 0 && stamp.lo == 123456789);
        DeliverBuffer(lastEngine, 1, NULL, output, 125456789);
        assert(d.getSamplePosition(&position, &stamp) == ASE_OK && position.lo == 96 && stamp.lo == 125456789);
        assert(d.stop() == ASE_OK && d.start() == ASE_OK);
        d.getSamplePosition(&position, &stamp);
        assert(position.lo == 0 && stamp.lo == 0);
    }
    {
        MiniAsioDriver first, second;
        ASIOCallbacks cb = Callbacks();
        ASIOBufferInfo a = Buffer(), b = Buffer();
        timeSupported = resetSupported = true;
        assert(first.createBuffers(&a, 1, 96, &cb) == ASE_OK);
        assert(first.start() == ASE_OK);
        KSEngine* firstEngine = lastEngine;
        assert(second.createBuffers(&b, 1, 144, &cb) == ASE_OK);
        assert(second.start() == ASE_OK);
        DeliverBuffer(firstEngine, 0, NULL, NULL, 100);
        DeliverBuffer(firstEngine, 1, NULL, NULL, 200);
        ASIOSamples position;
        ASIOTimeStamp stamp;
        first.getSamplePosition(&position, &stamp);
        assert(position.lo == 96 && stamp.lo == 200);
        second.getSamplePosition(&position, &stamp);
        assert(position.lo == 0 && stamp.lo == 0);
        assert(timeCalls == 2);
        firstEngine->onError(firstEngine->callbackContext, ERROR_DEVICE_NOT_CONNECTED);
        char error[124];
        first.getErrorMessage(error);
        assert(strcmp(error, "No error") != 0 && resetCalls == 1);
    }
    assert(allocations == frees);
    assert(DllCanUnloadNow() == S_OK);
    IClassFactory* factory = NULL;
    assert(DllGetClassObject(CLSID_AsioForUmc22, IID_IClassFactory, (void**)&factory) == S_OK);
    assert(DllCanUnloadNow() == S_FALSE);
    IASIO* driver = NULL;
    assert(factory->CreateInstance(NULL, CLSID_AsioForUmc22, (void**)&driver) == S_OK);
    assert(driver->getDriverVersion() == 10001);
    factory->LockServer(TRUE);
    driver->Release();
    assert(DllCanUnloadNow() == S_FALSE);
    factory->LockServer(FALSE);
    factory->Release();
    assert(DllCanUnloadNow() == S_OK);
    assert(DllRegisterServer() == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    profileDeviceName = L"USB Audio CODEC";
    {
        MiniAsioDriver named;
        ASIOCallbacks cb = Callbacks();
        ASIOBufferInfo b = Buffer();
        assert(named.init(NULL) == ASIOTrue);
        assert(named.createBuffers(&b, 1, 96, &cb) == ASE_OK && wcscmp(lastDeviceName, L"USB Audio CODEC") == 0);
        assert(named.disposeBuffers() == ASE_OK);
    }
    static WCHAR longName[KS_DEVICE_NAME_CHARS + 8];
    for (int i = 0; i < KS_DEVICE_NAME_CHARS + 7; ++i) longName[i] = L'x';
    profileDeviceName = longName;
    { MiniAsioDriver truncated; assert(truncated.init(NULL) == ASIOFalse); }
    profileDeviceName = L"";
    {   // Without asio_settings.ini: 48 kHz and 96 samples.
        profileMissing = true;
        MiniAsioDriver d;
        long min, max, preferred, granularity;
        ASIOSampleRate rate;
        assert(d.init(NULL) == ASIOTrue && d.getSampleRate(&rate) == ASE_OK && rate == 48000);
        assert(d.getBufferSize(&min, &max, &preferred, &granularity) == ASE_OK && preferred == 96);
        profileMissing = false;
    }
    {   // Latency: input the device driver holds back, calibration offsets, internal buffering.
        engineInputDelay = 480;
        profileInputOffset = L"250";
        profileOutputOffset = L"-500";
        MiniAsioDriver d;
        ASIOCallbacks cb = Callbacks();
        ASIOBufferInfo b = Buffer();
        assert(d.init(NULL) == ASIOTrue && d.createBuffers(&b, 1, 96, &cb) == ASE_OK);
        long input, output;
        assert(d.getLatencies(&input, &output) == ASE_OK);
        assert(input == 96 + 584 + 12 && output == 96 - 24); // 12.17 ms = 584; 250 us = 12, -500 us = -24.
        ASIOInternalBufferInfo internal = {};
        assert(d.future(kAsioGetInternalBufferSamples, &internal) == ASE_SUCCESS);
        assert(internal.inputSamples == 596 && internal.outputSamples == 0);
        assert(d.future(kAsioGetInternalBufferSamples, NULL) == ASE_InvalidParameter);
        assert(d.future(kAsioCanTimeInfo, NULL) == ASE_SUCCESS);
        assert(d.future(kAsioCanReportOverload, NULL) == ASE_SUCCESS);
        assert(d.future(kAsioCanInputMonitor, NULL) == ASE_NotPresent);
        assert(d.disposeBuffers() == ASE_OK);
        engineInputDelay = 0;
        profileInputOffset = profileOutputOffset = L"";
    }
    for (const WCHAR* bad : {L"abc", L"12x", L"2000000"}) {
        profileOutputOffset = bad;
        MiniAsioDriver invalid;
        assert(invalid.init(NULL) == ASIOFalse);
    }
    profileOutputOffset = L"";
    {   // Dropouts reach the host as overloads; positions include the output time they lost.
        overloadSupported = true;
        MiniAsioDriver d;
        ASIOCallbacks cb = Callbacks();
        ASIOBufferInfo b = Buffer();
        assert(d.createBuffers(&b, 1, 96, &cb) == ASE_OK && d.start() == ASE_OK);
        int16_t output[192];
        ASIOSamples position;
        ASIOTimeStamp stamp;
        DeliverBuffer(lastEngine, 0, NULL, output, 100);
        assert(overloadCalls == 0);
        lastEngine->dropouts = 1;
        lastEngine->slipSamples = 500;
        DeliverBuffer(lastEngine, 1, NULL, output, 200);
        assert(overloadCalls == 1);
        assert(d.getSamplePosition(&position, &stamp) == ASE_OK && position.lo == 96 + 500 && stamp.lo == 200);
        DeliverBuffer(lastEngine, 0, NULL, output, 300);
        assert(overloadCalls == 1);
        assert(d.getSamplePosition(&position, &stamp) == ASE_OK && position.lo == 2 * 96 + 500);
        overloadSupported = false;
    }
    {   // Device errors get clear text, and a dialog once per driver instance.
        MiniAsioDriver d;
        ASIOCallbacks cb = Callbacks();
        ASIOBufferInfo b = Buffer();
        char message[124];
        failInit = true;
        initError = KS_ERROR_DEVICE_IN_USE;
        assert(d.createBuffers(&b, 1, 96, &cb) == ASE_HWMalfunction && messageBoxes == 1);
        d.getErrorMessage(message);
        assert(strstr(message, "in use by another program"));
        initError = KS_ERROR_DEVICE_NOT_FOUND;
        assert(d.createBuffers(&b, 1, 96, &cb) == ASE_HWMalfunction && messageBoxes == 1);
        d.getErrorMessage(message);
        assert(strstr(message, "not found"));
        initError = ERROR_NOT_FOUND;
        failInit = false;
    }
    {   // Control panel: a chosen buffer size is offered, saved, and the host asked to reopen.
        resetSupported = true;
        MiniAsioDriver d;
        ASIOCallbacks cb = Callbacks();
        ASIOBufferInfo b = Buffer();
        long min, max, preferred, granularity;
        assert(d.createBuffers(&b, 1, 96, &cb) == ASE_OK);
        int resets = resetCalls, boxes = messageBoxes;
        dialogChoice = 144;
        dialogResult = IDCANCEL;
        assert(d.controlPanel() == ASE_OK && dialogs == 1 && !savedBufferSize[0] && resetCalls == resets);
        dialogResult = IDOK;
        assert(d.controlPanel() == ASE_OK && dialogs == 2 && wcscmp(savedBufferSize, L"144") == 0);
        assert(resetCalls == resets + 1 && messageBoxes == boxes);
        assert(d.getBufferSize(&min, &max, &preferred, &granularity) == ASE_OK && preferred == 144);
        assert(!d.SetPreferredBufferSize(100) && !d.SetPreferredBufferSize(1056));
        assert(d.disposeBuffers() == ASE_OK);
        dialogResult = IDCANCEL;
    }
    {   // Extra output buffers: read from the INI, given to the engine, and reported as
        // output latency and internal buffering. The panel changes them for the next
        // time buffers are created, with a single reset request.
        profileExtra = L"1";
        resetSupported = true;
        MiniAsioDriver d;
        ASIOCallbacks cb = Callbacks();
        ASIOBufferInfo b = Buffer();
        long input, output;
        assert(d.init(NULL) == ASIOTrue && d.getLatencies(&input, &output) == ASE_OK && output == 96 * 2);
        assert(d.createBuffers(&b, 1, 144, &cb) == ASE_OK && lastExtraSet == 1 && lastEngine->renderPackets == 3);
        assert(d.getLatencies(&input, &output) == ASE_OK && output == 144 * 2);
        ASIOInternalBufferInfo internal = {};
        assert(d.future(kAsioGetInternalBufferSamples, &internal) == ASE_SUCCESS && internal.outputSamples == 144);
        int resets = resetCalls;
        savedBufferSize[0] = savedExtra[0] = 0;
        dialogResult = IDOK;
        dialogChoice = 96; // The preferred size from the INI: unchanged.
        dialogExtra = 2;
        assert(d.controlPanel() == ASE_OK && strstr(panelText, "Output queue: 2 buffer(s) ahead, 1 extra"));
        assert(wcscmp(savedExtra, L"2") == 0 && !savedBufferSize[0] && resetCalls == resets + 1);
        assert(d.getLatencies(&input, &output) == ASE_OK && output == 144 * 2); // Until the host reopens.
        assert(d.disposeBuffers() == ASE_OK && d.createBuffers(&b, 1, 144, &cb) == ASE_OK && lastExtraSet == 2);
        assert(d.getLatencies(&input, &output) == ASE_OK && output == 144 * 3);
        resets = resetCalls;
        dialogChoice = 192;
        dialogExtra = 0;
        assert(d.controlPanel() == ASE_OK && wcscmp(savedBufferSize, L"192") == 0 && wcscmp(savedExtra, L"0") == 0);
        assert(resetCalls == resets + 1); // Both settings changed, one reset.
        assert(!d.ApplySettings(96, KS_MAX_EXTRA_OUTPUT_BUFFERS + 1) && !d.ApplySettings(96, -1));
        assert(d.disposeBuffers() == ASE_OK);
        dialogResult = IDCANCEL;
        dialogExtra = -1;
    }
    for (const WCHAR* bad : {L"3", L"-1", L"x"}) {
        profileExtra = bad;
        MiniAsioDriver invalid;
        assert(invalid.init(NULL) == ASIOFalse);
    }
    profileExtra = L"";
    {   // The real panel dialog, created without being shown: the offered choices, the
        // current selections, the choices returned by OK, and Cancel returning nothing.
        for (int ok = 0; ok < 2; ++ok) {
            panel::Context context("diagnostics", 48000.0, 96, 1);
            context.Add("Device", 0, "Device", panel::kPlain, "%s", "USB Audio CODEC");
            context.Add("This session", 1, "Output dropouts", panel::kWarning, "%d", 3);
            panel::Template dialog(L"ASIO-for-UMC22");
            panel::Build(&dialog, context);
            HWND window = CreateDialogIndirectParamW(GetModuleHandleW(NULL), dialog.Data(), NULL, panel::Proc,
                                                     (LPARAM)&context);
            assert(window);
            char shown[32];
            GetDlgItemTextA(window, panel::kFirstValue + 1, shown, sizeof(shown));
            assert(!strcmp(shown, "3") && GetDlgItem(window, panel::kFirstGroup + 2)); // Two groups, then Settings.
            HWND sizes = GetDlgItem(window, panel::kBufferSize), safety = GetDlgItem(window, panel::kSafety);
            assert(SendMessageA(sizes, CB_GETCOUNT, 0, 0) == 21 && SendMessageA(safety, CB_GETCOUNT, 0, 0) == 3);
            assert(SendMessageA(sizes, CB_GETITEMDATA, SendMessageA(sizes, CB_GETCURSEL, 0, 0), 0) == 96);
            assert(SendMessageA(safety, CB_GETITEMDATA, SendMessageA(safety, CB_GETCURSEL, 0, 0), 0) == 1);
            SendMessageA(sizes, CB_SETCURSEL, 0, 0);  // 48 samples.
            SendMessageA(safety, CB_SETCURSEL, 2, 0); // Two extra output buffers.
            SendMessageA(window, WM_COMMAND, ok ? IDOK : IDCANCEL, 0);
            assert(context.bufferSize == (ok ? 48 : 96) && context.extraOutputBuffers == (ok ? 2 : 1));
            DestroyWindow(window);
        }
    }
    // Capture hints are opt-in and rejection does not break initialization or
    // claim a shorter latency. Accepted 3 ms requests report the delay measured with
    // input alignment for buffers up to 3 ms. The panel distinguishes the outcomes.
    for (DWORD error : {DWORD(ERROR_SUCCESS), DWORD(ERROR_NOT_SUPPORTED)}) {
        profileCaptureBuffer = L"3";
        captureHintError = error;
        engineInputDelay = 480;
        MiniAsioDriver d;
        ASIOCallbacks cb = Callbacks();
        ASIOBufferInfo b = Buffer();
        assert(d.init(NULL) == ASIOTrue && d.createBuffers(&b, 1, 96, &cb) == ASE_OK);
        assert(lastEngine->captureBufferMs == 3 && lastEngine->captureBufferError == error);
        long input, output;
        assert(d.getLatencies(&input, &output) == ASE_OK && input == (error ? 96 + 584 : 96 + 96 + 272) && output == 96);
        assert(d.controlPanel() == ASE_OK);
        assert(strstr(panelText, error ? "3 ms rejected" : "3 ms accepted"));
        // Alignment keeps one buffer less when a 3 ms block fills exactly one buffer
        // (144 samples at 48 kHz), and gains nothing that was measured at 4 ms.
        assert(d.disposeBuffers() == ASE_OK && d.createBuffers(&b, 1, 144, &cb) == ASE_OK);
        assert(d.getLatencies(&input, &output) == ASE_OK && input == (error ? 144 + 584 : 144 + 272));
        assert(d.disposeBuffers() == ASE_OK && d.createBuffers(&b, 1, 192, &cb) == ASE_OK);
        assert(d.getLatencies(&input, &output) == ASE_OK && input == 192 + 584);
        assert(d.start() == ASE_OK && d.stop() == ASE_OK);
    }
    for (const WCHAR* invalid : {L"-1", L"1", L"2", L"11", L"3ms", L"abc", L"99999999999999999999999999999999999"}) {
        profileCaptureBuffer = invalid;
        MiniAsioDriver d;
        assert(d.init(NULL) == ASIOFalse);
    }
    profileCaptureBuffer = L"";
    captureHintError = 0;
    engineInputDelay = 0;
    {   // After the input stage the kernel can overwrite its packet; host input
        // must remain intact in both halves of the ASIO double buffer.
        timeSupported = false;
        MiniAsioDriver d;
        ASIOCallbacks cb = Callbacks();
        ASIOBufferInfo b[] = {Buffer(0, ASIOTrue), Buffer(1, ASIOTrue), Buffer(0), Buffer(1)};
        assert(d.createBuffers(b, 4, 96, &cb) == ASE_OK && d.start() == ASE_OK);
        assert(lastEngine->captureBufferMs == 0);
        for (int half = 0; half < 2; ++half) {
            int16_t input[192], output[192];
            for (int i = 0; i < 96; ++i) { input[2*i] = 8192; input[2*i+1] = -16384; }
            lastEngine->onInputReady(lastEngine->callbackContext, half, input);
            memset(input, 0x55, sizeof(input));
            lastEngine->onBufferReady(lastEngine->callbackContext, half, output, 100 + half);
            for (int i = 0; i < 96; ++i) {
                assert(((float*)b[0].buffers[half])[i] == 0.25f);
                assert(((float*)b[1].buffers[half])[i] == -0.5f);
            }
        }
    }
    assert(allocations == frees);
    profileBuffer = 0;
    { MiniAsioDriver invalid; assert(invalid.init(NULL) == ASIOFalse); }
    puts("PASS ASIO lifecycle: validation, allocation rollback, rates, device name, latency reporting, "
         "extra output buffers, dropout reports, error messages, callbacks, timing, instance isolation, COM and registration errors");
}
