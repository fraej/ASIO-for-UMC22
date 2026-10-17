#include "asio_types.h"
#include "ks_engine.h"
#include "audio_convert.h"
#include <atomic>
#include <stdarg.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <wchar.h>

static const GUID CLSID_AsioForUmc22 = {
    0xb2a0f51e, 0x32e8, 0x48d3, { 0x89, 0x02, 0x6c, 0xd4, 0x52, 0xd7, 0xef, 0x31 }
};
#define DRIVER_VERSION_TEXT "1.0.1"
#define DRIVER_VERSION_TEXT_W L"1.0.1"
static HMODULE g_hModule = NULL;
static volatile LONG g_objectCount = 0;
static volatile LONG g_serverLocks = 0;

// The control panel: diagnostics, and the buffer size and output safety choices.
// Many hosts have no buffer-size setting for ASIO and open this panel instead.
// The dialog is built in memory, so the DLL needs no resource file.
namespace panel {
enum {
    kBufferSize = 101, kSafety = 102, kCopy = 103, kTitle = 104, kSummary = 105,
    kFirstLabel = 200, kFirstValue = 300, kFirstGroup = 400, kStatic = 0xFFFF
};
enum Tone { kPlain, kGood, kWarning };
enum { kMaxRows = 24 };

struct Row {
    const char* section; // Starts a new group with this title; NULL continues the current one.
    int column;          // 0 or 1; read where a group starts.
    const char* label;
    char value[96];
    int tone;
};

struct Context {
    const char* text; // The full report, copied to the clipboard.
    char summary[128];
    int summaryTone;
    Row rows[kMaxRows];
    int rowCount;
    double sampleRate;
    long bufferSize;         // In: current preferred size. Out: the chosen size.
    long extraOutputBuffers; // In: current setting. Out: the chosen setting.
    HFONT titleFont, boldFont; // Owned by the dialog while it is open.

    Context(const char* report, double rate, long size, long extra)
        : text(report), summaryTone(kPlain), rowCount(0), sampleRate(rate), bufferSize(size),
          extraOutputBuffers(extra), titleFont(NULL), boldFont(NULL) { summary[0] = 0; }
    void Add(const char* section, int column, const char* label, int tone, const char* format, ...)
        __attribute__((format(printf, 6, 7))) {
        if (rowCount >= kMaxRows) return;
        Row& row = rows[rowCount++];
        row.section = section;
        row.column = column;
        row.label = label;
        row.tone = tone;
        va_list args;
        va_start(args, format);
        vsnprintf(row.value, sizeof(row.value), format, args);
        va_end(args);
    }
};

class Template {
    std::vector<WORD> m_words;
    void Dword(DWORD value) { m_words.push_back(LOWORD(value)); m_words.push_back(HIWORD(value)); }
    void Text(const WCHAR* text) { do m_words.push_back(*text); while (*text++); }
    void Rect(short x, short y, short cx, short cy) {
        for (short value : {x, y, cx, cy}) m_words.push_back((WORD)value);
    }
public:
    explicit Template(const WCHAR* title) {
        Dword(DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
        Dword(0);
        m_words.push_back(0); // Item count, raised by Item().
        Rect(0, 0, 0, 0);     // Set by Size().
        m_words.push_back(0); // No menu.
        m_words.push_back(0); // Default dialog class.
        Text(title);
        m_words.push_back(9);
        Text(L"Segoe UI");
    }
    void Size(short cx, short cy) { m_words[7] = (WORD)cx; m_words[8] = (WORD)cy; }
    void Item(DWORD style, short x, short y, short cx, short cy, WORD id, WORD controlClass, const WCHAR* text) {
        if (m_words.size() % 2) m_words.push_back(0); // Items start on DWORD boundaries.
        Dword(style | WS_CHILD | WS_VISIBLE);
        Dword(0);
        Rect(x, y, cx, cy);
        m_words.push_back(id);
        m_words.push_back(0xFFFF);
        m_words.push_back(controlClass);
        Text(text);
        m_words.push_back(0); // No creation data.
        ++m_words[4];
    }
    const DLGTEMPLATE* Data() const { return reinterpret_cast<const DLGTEMPLATE*>(m_words.data()); }
};

const WORD kButton = 0x0080, kStaticClass = 0x0082, kCombo = 0x0085;

// Layout in dialog units: two columns of groups with label/value rows, then the settings.
const short kMargin = 7, kColumnWidth = 216, kGap = 6, kWidth = 2 * kMargin + 2 * kColumnWidth + kGap;
const short kRowHeight = 11, kGroupTop = 13, kGroupBottom = 5, kGroupsTop = 42, kLabelWidth = 70;

void Build(Template* dialog, const Context& context) {
    dialog->Item(SS_LEFT | SS_NOPREFIX, kMargin + 2, 6, kWidth - 2 * kMargin, 16, kTitle, kStaticClass,
                 L"ASIO-for-UMC22 " DRIVER_VERSION_TEXT_W);
    dialog->Item(SS_LEFT | SS_NOPREFIX, kMargin + 2, 23, kWidth - 2 * kMargin, 10, kSummary, kStaticClass, L"");
    dialog->Item(SS_ETCHEDHORZ, kMargin, 36, kWidth - 2 * kMargin, 1, kStatic, kStaticClass, L"");

    // Group heights: each column's last group stretches so both columns end together.
    struct Group { int first, count, column; short y, height; } groups[kMaxRows];
    int groupCount = 0;
    for (int i = 0; i < context.rowCount; ++i) {
        if (context.rows[i].section || !groupCount)
            groups[groupCount++] = {i, 0, context.rows[i].section ? context.rows[i].column : 0, 0, 0};
        ++groups[groupCount - 1].count;
    }
    short bottom[2] = {kGroupsTop, kGroupsTop};
    int last[2] = {-1, -1};
    for (int g = 0; g < groupCount; ++g) {
        Group& group = groups[g];
        group.y = bottom[group.column];
        group.height = (short)(kGroupTop + group.count * kRowHeight + kGroupBottom);
        bottom[group.column] = (short)(group.y + group.height + 4);
        last[group.column] = g;
    }
    short end = bottom[0] > bottom[1] ? bottom[0] : bottom[1];
    for (int column = 0; column < 2; ++column)
        if (last[column] >= 0) groups[last[column]].height = (short)(groups[last[column]].height + end - bottom[column]);

    for (int g = 0; g < groupCount; ++g) {
        const Group& group = groups[g];
        short x = (short)(kMargin + group.column * (kColumnWidth + kGap));
        WCHAR title[64] = L"";
        if (context.rows[group.first].section)
            MultiByteToWideChar(CP_ACP, 0, context.rows[group.first].section, -1, title, 64);
        dialog->Item(BS_GROUPBOX, x, group.y, kColumnWidth, group.height, (WORD)(kFirstGroup + g), kButton, title);
        for (int r = 0; r < group.count; ++r) {
            int i = group.first + r;
            short y = (short)(group.y + kGroupTop + r * kRowHeight);
            dialog->Item(SS_LEFT | SS_NOPREFIX, (short)(x + 8), y, kLabelWidth, 9, (WORD)(kFirstLabel + i),
                         kStaticClass, L"");
            dialog->Item(SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, (short)(x + 8 + kLabelWidth + 4), y,
                         (short)(kColumnWidth - kLabelWidth - 20), 9, (WORD)(kFirstValue + i), kStaticClass, L"");
        }
    }

    short y = end;
    short settingsWidth = kWidth - 2 * kMargin, half = (short)(kMargin + kColumnWidth + kGap);
    dialog->Item(BS_GROUPBOX, kMargin, y, settingsWidth, 54, (WORD)(kFirstGroup + groupCount), kButton, L"Settings");
    dialog->Item(SS_LEFT, kMargin + 8, y + 16, 52, 9, kStatic, kStaticClass, L"Buffer size:");
    dialog->Item(CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kMargin + 62, y + 14, 144, 150, kBufferSize, kCombo, L"");
    dialog->Item(SS_LEFT, half + 8, y + 16, 52, 9, kStatic, kStaticClass, L"Output safety:");
    dialog->Item(CBS_DROPDOWNLIST | WS_TABSTOP, half + 62, y + 14, 144, 60, kSafety, kCombo, L"");
    dialog->Item(SS_LEFT, kMargin + 8, y + 32, settingsWidth - 16, 18, kStatic, kStaticClass,
                 L"Extra output buffers prevent crackles when the host is sometimes late, at the cost of "
                 L"output latency. Changes are saved to asio_settings.ini, and the host is asked to reopen "
                 L"the driver with them.");
    y = (short)(y + 54 + 8);
    dialog->Item(BS_PUSHBUTTON | WS_TABSTOP, kMargin, y, 64, 14, kCopy, kButton, L"Copy details");
    dialog->Item(BS_DEFPUSHBUTTON | WS_TABSTOP, kWidth - kMargin - 50 - 6 - 50, y, 50, 14, IDOK, kButton, L"OK");
    dialog->Item(BS_PUSHBUTTON | WS_TABSTOP, kWidth - kMargin - 50, y, 50, 14, IDCANCEL, kButton, L"Cancel");
    dialog->Size(kWidth, (short)(y + 14 + kMargin));
}

COLORREF ToneColor(int tone) {
    if (tone == kGood) return RGB(16, 124, 16);
    if (tone == kWarning) return RGB(196, 80, 0);
    return GetSysColor(COLOR_WINDOWTEXT);
}

// Puts the report on the clipboard with Windows line endings.
bool CopyReport(HWND owner, const char* text) {
    size_t size = 1;
    for (const char* c = text; *c; ++c) size += *c == '\n' ? 2 : 1;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory) return false;
    char* out = static_cast<char*>(GlobalLock(memory));
    for (const char* c = text; *c; ++c) {
        if (*c == '\n') *out++ = '\r';
        *out++ = *c;
    }
    *out = 0;
    GlobalUnlock(memory);
    if (!OpenClipboard(owner)) { GlobalFree(memory); return false; }
    EmptyClipboard();
    bool copied = SetClipboardData(CF_TEXT, memory) != NULL;
    if (!copied) GlobalFree(memory);
    CloseClipboard();
    return copied;
}

INT_PTR CALLBACK Proc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    Context* context = reinterpret_cast<Context*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        context = reinterpret_cast<Context*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        LOGFONTW font;
        HFONT dialogFont = (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0);
        if (dialogFont && GetObjectW(dialogFont, sizeof(font), &font)) {
            font.lfWeight = FW_SEMIBOLD;
            context->boldFont = CreateFontIndirectW(&font);
            font.lfHeight = font.lfHeight * 3 / 2;
            context->titleFont = CreateFontIndirectW(&font);
        }
        if (context->titleFont) SendDlgItemMessageW(dialog, kTitle, WM_SETFONT, (WPARAM)context->titleFont, FALSE);
        SetDlgItemTextA(dialog, kSummary, context->summary);
        if (context->boldFont) SendDlgItemMessageW(dialog, kSummary, WM_SETFONT, (WPARAM)context->boldFont, FALSE);
        for (int i = 0; i < context->rowCount; ++i) {
            SetDlgItemTextA(dialog, kFirstLabel + i, context->rows[i].label);
            SetDlgItemTextA(dialog, kFirstValue + i, context->rows[i].value);
        }
        for (int g = kFirstGroup; GetDlgItem(dialog, g); ++g)
            if (context->boldFont) SendDlgItemMessageW(dialog, g, WM_SETFONT, (WPARAM)context->boldFont, FALSE);
        HWND list = GetDlgItem(dialog, kBufferSize);
        for (long size = KS_MIN_BUFFER_SAMPLES; size <= KS_MAX_BUFFER_SAMPLES; size += KS_BUFFER_GRANULARITY) {
            char label[48];
            snprintf(label, sizeof(label), "%ld samples (%.2f ms)", size, size * 1000.0 / context->sampleRate);
            LRESULT item = SendMessageA(list, CB_ADDSTRING, 0, (LPARAM)label);
            SendMessageA(list, CB_SETITEMDATA, item, size);
            if (size == context->bufferSize) SendMessageA(list, CB_SETCURSEL, item, 0);
        }
        static const char* const choices[] = {"None (lowest latency)", "1 extra buffer (+1 buffer of latency)",
                                              "2 extra buffers (+2 buffers of latency)"};
        static_assert(sizeof(choices) / sizeof(choices[0]) == KS_MAX_EXTRA_OUTPUT_BUFFERS + 1, "one label per setting");
        HWND safety = GetDlgItem(dialog, kSafety);
        for (long extra = 0; extra <= KS_MAX_EXTRA_OUTPUT_BUFFERS; ++extra) {
            LRESULT item = SendMessageA(safety, CB_ADDSTRING, 0, (LPARAM)choices[extra]);
            SendMessageA(safety, CB_SETITEMDATA, item, extra);
            if (extra == context->extraOutputBuffers) SendMessageA(safety, CB_SETCURSEL, item, 0);
        }
        return TRUE;
    }
    if (!context) return FALSE;
    if (message == WM_CTLCOLORSTATIC) {
        int id = GetDlgCtrlID((HWND)lParam);
        COLORREF color;
        if (id == kSummary) color = ToneColor(context->summaryTone);
        else if (id >= kFirstValue && id < kFirstValue + context->rowCount) color = ToneColor(context->rows[id - kFirstValue].tone);
        else if (id >= kFirstLabel && id < kFirstLabel + context->rowCount) color = GetSysColor(COLOR_GRAYTEXT);
        else return FALSE;
        SetTextColor((HDC)wParam, color);
        SetBkMode((HDC)wParam, TRANSPARENT);
        return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
    }
    if (message == WM_COMMAND && LOWORD(wParam) == kCopy) {
        SetDlgItemTextW(dialog, kCopy, CopyReport(dialog, context->text) ? L"Copied" : L"Copy failed");
        return TRUE;
    }
    if (message == WM_COMMAND && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)) {
        HWND list = GetDlgItem(dialog, kBufferSize), safety = GetDlgItem(dialog, kSafety);
        LRESULT item = SendMessageA(list, CB_GETCURSEL, 0, 0), extra = SendMessageA(safety, CB_GETCURSEL, 0, 0);
        if (LOWORD(wParam) == IDOK && item != CB_ERR)
            context->bufferSize = (long)SendMessageA(list, CB_GETITEMDATA, item, 0);
        if (LOWORD(wParam) == IDOK && extra != CB_ERR)
            context->extraOutputBuffers = (long)SendMessageA(safety, CB_GETITEMDATA, extra, 0);
        EndDialog(dialog, LOWORD(wParam));
        return TRUE;
    }
    if (message == WM_DESTROY) {
        if (context->titleFont) DeleteObject(context->titleFont);
        if (context->boldFont) DeleteObject(context->boldFont);
        context->titleFont = context->boldFont = NULL;
    }
    return FALSE;
}
}  // namespace panel

class MiniAsioDriver : public IASIO {
    LONG m_refCount;
    KSEngine m_engine;
    ASIOSampleRate m_sampleRate;
    long m_bufferSize;    // Size of the current buffers.
    long m_preferredSize; // Size offered to the host; set in the INI or the control panel.
    ASIOCallbacks m_callbacks;
    HWND m_window;        // The host's window, from init(), owns the control panel.
    WCHAR m_settingsPath[MAX_PATH];
    float* m_hostMemory;
    float* m_hostInputBuffers[2][2];
    float* m_hostOutputBuffers[2][2];
    bool m_active[2][2]; // [input/output][channel]
    bool m_hostLocked;
    bool m_buffersReady;
    bool m_started;
    bool m_timeInfoSupported;
    bool m_resetSupported;
    bool m_overloadSupported;
    bool m_configurationValid;
    bool m_errorShown;
    long m_hostVersion;
    long m_inputOffsetUs, m_outputOffsetUs; // Loopback calibration from the INI.
    long m_captureBufferMs; // 0 leaves Windows' USB capture duration unchanged.
    long m_extraOutputBuffers; // Output buffers queued beyond the one playing.
    WCHAR m_deviceName[KS_DEVICE_NAME_CHARS]; // Empty selects the first UMC22 hardware ID.
    LONG64 m_dropoutsSeen, m_slipSeen;
    uint64_t m_nextSamplePosition;
    std::atomic<unsigned> m_positionSequence;
    std::atomic<uint64_t> m_samplePosition;
    std::atomic<uint64_t> m_timestamp;
    std::atomic<DWORD> m_error;

    static bool ValidBufferSize(long size) {
        return size >= KS_MIN_BUFFER_SAMPLES && size <= KS_MAX_BUFFER_SAMPLES &&
               size % KS_BUFFER_GRANULARITY == 0;
    }

    ASIOError Fail(ASIOError result, DWORD error) {
        m_error.store(error ? error : ERROR_GEN_FAILURE);
        return result;
    }

    // Clearer text for failures that Windows describes vaguely (at most 123 characters).
    static const char* DriverErrorText(DWORD error) {
        switch (error) {
        case KS_ERROR_DEVICE_IN_USE:
            return "UMC22 is in use by another program. Close it, or switch that program to another audio device.";
        case KS_ERROR_DEVICE_NOT_FOUND:
            return "UMC22 not found. Check the USB connection, or the DeviceName setting in asio_settings.ini.";
        case KS_ERROR_FORMAT_UNSUPPORTED:
            return "The audio device does not accept 16-bit stereo at the selected sample rate.";
        default:
            return NULL;
        }
    }

    // Optional signed integer settings; callers apply any narrower bounds.
    static bool ReadSetting(const WCHAR* path, const WCHAR* key, long* value) {
        WCHAR text[32];
        DWORD length = GetPrivateProfileStringW(L"Audio", key, L"", text, 32, path);
        WCHAR* end = text;
        long parsed = wcstol(text, &end, 10);
        if (length == 31 || (text[0] && (*end || parsed < -1000000 || parsed > 1000000))) return false;
        *value = parsed;
        return true;
    }

    // Input delay beyond the current buffer under Windows' usbaudio driver, measured
    // through a loopback cable on a UMC22 with both converters included: 12.17 ms with
    // Windows' default capture. With accepted 3 ms capture requests, input alignment
    // leaves one more buffer plus 5.67 ms for buffers up to 2.5 ms, and only 5.67 ms
    // when one 3 ms block fills exactly one buffer (144 samples at 48 kHz). Other
    // buffers varied between those and the default, so they report the default, as do
    // unmeasured request durations. Each start can land half a buffer either side.
    // Other device drivers leave inputDelaySamples at 0 and rely on the offsets.
    bool AlignedCapture() const {
        return m_captureBufferMs == 3 && m_engine.hCapturePin && !m_engine.captureBufferError &&
               (m_bufferSize * 1000.0 <= 2.5 * m_sampleRate || m_bufferSize * 1000.0 == 3.0 * m_sampleRate);
    }
    long InputDelaySamples() const {
        if (!m_engine.inputDelaySamples) return 0;
        long fixed = (long)(5.67 * m_sampleRate / 1000.0 + 0.5);
        if (AlignedCapture()) return (m_bufferSize * 1000.0 <= 2.5 * m_sampleRate ? m_bufferSize : 0) + fixed;
        return (long)(12.17 * m_sampleRate / 1000.0 + 0.5);
    }

    long WithOffset(long samples, long offsetUs) const {
        double result = samples + offsetUs * m_sampleRate / 1000000.0;
        return result > 0 ? (long)(result + 0.5) : 0;
    }

    void PublishPosition(uint64_t position, uint64_t timestamp) {
        // Atomic fields and sequence prevent a reader pairing a new position
        // with an old timestamp. The audio writer never waits for readers.
        m_positionSequence.fetch_add(1);
        m_samplePosition.store(position);
        m_timestamp.store(timestamp);
        m_positionSequence.fetch_add(1);
    }

    void FreeHostBuffers() {
        if (m_hostLocked) VirtualUnlock(m_hostMemory, m_bufferSize * sizeof(float) * 8);
        if (m_hostMemory) VirtualFree(m_hostMemory, 0, MEM_RELEASE);
        m_hostMemory = NULL;
        m_hostLocked = false;
        ZeroMemory(m_hostInputBuffers, sizeof(m_hostInputBuffers));
        ZeroMemory(m_hostOutputBuffers, sizeof(m_hostOutputBuffers));
        ZeroMemory(m_active, sizeof(m_active));
    }

public:
    MiniAsioDriver() : m_refCount(1), m_sampleRate(48000), m_bufferSize(96), m_preferredSize(96), m_window(NULL),
        m_hostMemory(NULL),
        m_hostLocked(false), m_buffersReady(false), m_started(false), m_timeInfoSupported(false),
        m_resetSupported(false), m_overloadSupported(false), m_configurationValid(true), m_errorShown(false),
        m_hostVersion(0), m_inputOffsetUs(0), m_outputOffsetUs(0), m_captureBufferMs(0), m_extraOutputBuffers(0), m_dropoutsSeen(0), m_slipSeen(0),
        m_nextSamplePosition(0), m_positionSequence(0), m_samplePosition(0), m_timestamp(0), m_error(ERROR_SUCCESS) {
        InterlockedIncrement(&g_objectCount);
        ZeroMemory(&m_engine, sizeof(m_engine));
        ZeroMemory(&m_callbacks, sizeof(m_callbacks));
        ZeroMemory(m_hostInputBuffers, sizeof(m_hostInputBuffers));
        ZeroMemory(m_hostOutputBuffers, sizeof(m_hostOutputBuffers));
        ZeroMemory(m_active, sizeof(m_active));
        ZeroMemory(m_deviceName, sizeof(m_deviceName));
        ZeroMemory(m_settingsPath, sizeof(m_settingsPath));
        bool settingsValid = true;
        WCHAR* path = m_settingsPath;
        DWORD length = GetModuleFileNameW(g_hModule, path, MAX_PATH);
        if (length && length < MAX_PATH) {
            WCHAR* slash = wcsrchr(path, L'\\');
            if (!slash || (slash - path) + 1 + wcslen(L"asio_settings.ini") >= MAX_PATH) path[0] = 0;
            else {
                wcscpy(slash + 1, L"asio_settings.ini");
                m_sampleRate = GetPrivateProfileIntW(L"Audio", L"SampleRate", 48000, path);
                m_bufferSize = (long)GetPrivateProfileIntW(L"Audio", L"BufferSize", 96, path);
                DWORD nameLength = GetPrivateProfileStringW(L"Audio", L"DeviceName", L"", m_deviceName,
                                                            KS_DEVICE_NAME_CHARS, path);
                // A truncated name could match a different device than intended.
                settingsValid = nameLength < KS_DEVICE_NAME_CHARS - 1 &&
                                ReadSetting(path, L"InputLatencyOffset", &m_inputOffsetUs) &&
                                ReadSetting(path, L"OutputLatencyOffset", &m_outputOffsetUs) &&
                                ReadSetting(path, L"CaptureBufferMs", &m_captureBufferMs) &&
                                (m_captureBufferMs == 0 || (m_captureBufferMs >= 3 && m_captureBufferMs <= 10)) &&
                                ReadSetting(path, L"ExtraOutputBuffers", &m_extraOutputBuffers) &&
                                m_extraOutputBuffers >= 0 && m_extraOutputBuffers <= KS_MAX_EXTRA_OUTPUT_BUFFERS;
            }
        } else path[0] = 0;
        m_preferredSize = m_bufferSize;
        m_configurationValid = canSampleRate(m_sampleRate) == ASE_OK && ValidBufferSize(m_bufferSize) &&
                               settingsValid;
        if (!m_configurationValid) m_error.store(ERROR_INVALID_PARAMETER);
    }

    virtual ~MiniAsioDriver() {
        disposeBuffers();
        InterlockedDecrement(&g_objectCount);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IASIO) || IsEqualIID(riid, CLSID_AsioForUmc22)) {
            *ppv = static_cast<IASIO*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG count = InterlockedDecrement(&m_refCount);
        if (!count) delete this;
        return count;
    }

    ASIOBool init(void* sysHandle) override {
        m_window = static_cast<HWND>(sysHandle);
        return m_configurationValid ? ASIOTrue : ASIOFalse;
    }
    void getDriverName(char* name) override { if (name) strcpy(name, "ASIO-for-UMC22"); }
    long getDriverVersion() override { return 10001; } // 1.0.1
    void getErrorMessage(char* message) override {
        if (!message) return;
        DWORD error = m_error.load();
        if (!error) error = (DWORD)InterlockedCompareExchange(&m_engine.lastError, 0, 0);
        const char* text = DriverErrorText(error);
        if (!error) strcpy(message, "No error");
        else if (text) snprintf(message, 124, "%s", text);
        else if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                NULL, error, 0, message, 124, NULL))
            snprintf(message, 124, "ASIO-for-UMC22: Windows error %lu", error);
    }

    ASIOError start() override {
        if (!m_buffersReady) return Fail(ASE_InvalidMode, ERROR_INVALID_STATE);
        if (m_started) return InterlockedCompareExchange(&m_engine.isRunning, 0, 0) ? ASE_OK : ASE_HWMalfunction;
        m_nextSamplePosition = 0;
        m_dropoutsSeen = m_slipSeen = 0; // The engine resets its counters on start.
        PublishPosition(0, 0);
        m_error.store(ERROR_SUCCESS);
        m_started = true;
        if (!KSEngine_Start(&m_engine, StaticInputReady, StaticBufferReady, StaticError, this)) {
            m_started = false;
            return Fail(ASE_HWMalfunction, GetLastError());
        }
        return ASE_OK;
    }

    ASIOError stop() override {
        // Join even if the worker already exited after an I/O error.
        if (!KSEngine_Stop(&m_engine)) return Fail(ASE_InvalidMode, GetLastError());
        m_started = false;
        return ASE_OK;
    }

    ASIOError getChannels(long* inputs, long* outputs) override {
        if (!inputs || !outputs) return ASE_InvalidParameter;
        *inputs = *outputs = 2;
        return ASE_OK;
    }
    ASIOError getLatencies(long* input, long* output) override {
        if (!input || !output) return ASE_InvalidParameter;
        // Input waits for a full buffer plus the measured delay of the USB driver and
        // converters (InputDelaySamples). Output plays after the buffers queued ahead
        // of it: one, plus any extra output buffers. The INI offsets correct what a
        // loopback measurement (tools/latency_test) finds on other setups.
        long queued = m_buffersReady ? (long)m_engine.renderPackets - 1 : 1 + m_extraOutputBuffers;
        *input = WithOffset(m_bufferSize + InputDelaySamples(), m_inputOffsetUs);
        *output = WithOffset(m_bufferSize * queued, m_outputOffsetUs);
        return ASE_OK;
    }
    ASIOError getBufferSize(long* min, long* max, long* preferred, long* granularity) override {
        if (!min || !max || !preferred || !granularity) return ASE_InvalidParameter;
        *min = KS_MIN_BUFFER_SAMPLES;
        *max = KS_MAX_BUFFER_SAMPLES;
        *preferred = m_preferredSize;
        *granularity = KS_BUFFER_GRANULARITY;
        return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate rate) override {
        return rate == 44100.0 || rate == 48000.0 ? ASE_OK : ASE_NoClock;
    }
    ASIOError getSampleRate(ASIOSampleRate* rate) override {
        if (!rate) return ASE_InvalidParameter;
        *rate = m_sampleRate;
        return ASE_OK;
    }
    ASIOError setSampleRate(ASIOSampleRate rate) override {
        if (canSampleRate(rate) != ASE_OK) return ASE_NoClock;
        if (rate == m_sampleRate) return ASE_OK;
        // The host must stop and dispose buffers before opening pins at a new
        // rate. Never report a rate different from the existing pin format.
        if (m_buffersReady) return Fail(ASE_InvalidMode, ERROR_INVALID_STATE);
        m_sampleRate = rate;
        return ASE_OK;
    }
    ASIOError getClockSources(ASIOClockSource* clocks, long* count) override {
        if (!clocks || !count || *count < 1) return ASE_InvalidParameter;
        clocks[0].index = 0;
        clocks[0].associatedChannel = clocks[0].associatedGroup = -1;
        clocks[0].isCurrentSource = ASIOTrue;
        strcpy(clocks[0].name, "UMC22 Internal Clock");
        *count = 1;
        return ASE_OK;
    }
    ASIOError setClockSource(long index) override { return index == 0 ? ASE_OK : ASE_InvalidParameter; }
    ASIOError getSamplePosition(ASIOSamples* position, ASIOTimeStamp* timestamp) override {
        if (!position || !timestamp) return ASE_InvalidParameter;
        uint64_t samples, ns;
        for (;;) {
            unsigned sequence = m_positionSequence.load();
            if (sequence & 1) continue;
            samples = m_samplePosition.load();
            ns = m_timestamp.load();
            if (sequence == m_positionSequence.load()) break;
        }
        position->hi = (unsigned long)(samples >> 32);
        position->lo = (unsigned long)samples;
        timestamp->hi = (unsigned long)(ns >> 32);
        timestamp->lo = (unsigned long)ns;
        return ASE_OK;
    }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        if (!info || info->channel < 0 || info->channel >= 2 ||
            (info->isInput != ASIOTrue && info->isInput != ASIOFalse)) return ASE_InvalidParameter;
        info->isActive = m_active[info->isInput ? 0 : 1][info->channel] ? ASIOTrue : ASIOFalse;
        info->channelGroup = 0;
        info->type = ASIOSTFloat32LSB;
        const char* name = info->isInput ? (info->channel == 0 ? "In 1 (Mic/Line)" : "In 2 (Instrument)") :
                                          (info->channel == 0 ? "Out 1 (L)" : "Out 2 (R)");
        strcpy(info->name, name);
        return ASE_OK;
    }

    ASIOError createBuffers(ASIOBufferInfo* infos, long count, long size, ASIOCallbacks* callbacks) override {
        if (m_buffersReady || m_started) return Fail(ASE_InvalidMode, ERROR_INVALID_STATE);
        if (!m_configurationValid || !infos || !callbacks || count <= 0 || count > 4 || !ValidBufferSize(size))
            return Fail(ASE_InvalidParameter, ERROR_INVALID_PARAMETER);
        bool active[2][2] = {};
        for (long i = 0; i < count; ++i) {
            if (infos[i].channelNum < 0 || infos[i].channelNum >= 2 ||
                (infos[i].isInput != ASIOTrue && infos[i].isInput != ASIOFalse)) return ASE_InvalidParameter;
            int direction = infos[i].isInput ? 0 : 1;
            if (active[direction][infos[i].channelNum]) return ASE_InvalidParameter;
            active[direction][infos[i].channelNum] = true;
        }
        bool timeInfo = callbacks->bufferSwitchTimeInfo && callbacks->asioMessage &&
                        callbacks->asioMessage(kAsioSupportsTimeInfo, 0, NULL, NULL) == 1;
        if (!timeInfo && !callbacks->bufferSwitch) return ASE_InvalidParameter;
        for (long i = 0; i < count; ++i) infos[i].buffers[0] = infos[i].buffers[1] = NULL;
        m_bufferSize = size;
        const SIZE_T bytes = size * sizeof(float) * 8;
        m_hostMemory = (float*)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!m_hostMemory) return Fail(ASE_NoMemory, GetLastError());
        memset(m_hostMemory, 0, bytes);
        m_hostLocked = VirtualLock(m_hostMemory, bytes) != FALSE;
        for (int ch = 0; ch < 2; ++ch) {
            for (int half = 0; half < 2; ++half) {
                m_hostInputBuffers[ch][half] = m_hostMemory + (ch * 2 + half) * size;
                m_hostOutputBuffers[ch][half] = m_hostMemory + (4 + ch * 2 + half) * size;
            }
        }
        if (!KSEngine_Init(&m_engine, (ULONG)m_sampleRate, 2, 16, size, m_deviceName)) {
            DWORD error = GetLastError();
            FreeHostBuffers();
            // Many hosts show only a generic failure; say what to fix, once.
            const char* text = DriverErrorText(error);
            if (text && !m_errorShown) {
                m_errorShown = true;
                MessageBoxA(NULL, text, "ASIO-for-UMC22", MB_OK | MB_ICONWARNING | MB_TASKMODAL | MB_SETFOREGROUND | MB_TOPMOST);
            }
            return Fail(ASE_HWMalfunction, error);
        }
        // Unsupported capture hints are diagnostic only; retain the working
        // Windows defaults and latency estimate rather than failing ASIO init.
        KSEngine_SetCaptureBufferDuration(&m_engine, (ULONG)m_captureBufferMs);
        KSEngine_SetExtraOutputBuffers(&m_engine, (ULONG)m_extraOutputBuffers); // Validated with the INI.
        m_callbacks = *callbacks; // The host's callbacks structure need not outlive this call.
        m_timeInfoSupported = timeInfo;
        m_resetSupported = callbacks->asioMessage &&
            callbacks->asioMessage(kAsioSelectorSupported, kAsioResetRequest, NULL, NULL) == 1;
        m_overloadSupported = callbacks->asioMessage &&
            callbacks->asioMessage(kAsioSelectorSupported, kAsioOverload, NULL, NULL) == 1;
        m_hostVersion = callbacks->asioMessage ? callbacks->asioMessage(kAsioEngineVersion, 0, NULL, NULL) : 0;
        memcpy(m_active, active, sizeof(active));
        for (long i = 0; i < count; ++i) {
            float* (*buffers)[2] = infos[i].isInput ? m_hostInputBuffers : m_hostOutputBuffers;
            infos[i].buffers[0] = buffers[infos[i].channelNum][0];
            infos[i].buffers[1] = buffers[infos[i].channelNum][1];
        }
        m_buffersReady = true;
        m_error.store(ERROR_SUCCESS);
        return ASE_OK;
    }

    ASIOError disposeBuffers() override {
        ASIOError result = stop();
        if (result != ASE_OK) return result;
        if (!KSEngine_Cleanup(&m_engine)) return Fail(ASE_InvalidMode, GetLastError());
        FreeHostBuffers();
        ZeroMemory(&m_callbacks, sizeof(m_callbacks));
        m_buffersReady = m_timeInfoSupported = m_resetSupported = m_overloadSupported = false;
        m_hostVersion = 0;
        return ASE_OK;
    }

    ASIOError controlPanel() override {
        KSStats stats;
        KSEngine_GetStats(&m_engine, &stats);
        double millisecondsPerTick = stats.counterFrequency ? 1000.0 / stats.counterFrequency : 0;
        double millisecondsPerSample = 1000.0 / m_sampleRate;
        char device[KS_DEVICE_NAME_CHARS * 2], service[64], setting[KS_DEVICE_NAME_CHARS * 2], error[128];
        if (!m_engine.deviceName[0] || !WideCharToMultiByte(CP_ACP, 0, m_engine.deviceName, -1, device,
                                                            sizeof(device), NULL, NULL))
            strcpy(device, "(not open)");
        if (!m_engine.deviceService[0] || !WideCharToMultiByte(CP_ACP, 0, m_engine.deviceService, -1, service,
                                                               sizeof(service), NULL, NULL))
            strcpy(service, "unknown");
        if (!m_deviceName[0] || !WideCharToMultiByte(CP_ACP, 0, m_deviceName, -1, setting,
                                                     sizeof(setting), NULL, NULL))
            strcpy(setting, "(empty: first VID_08BB&PID_2902 device)");
        long input, output;
        getLatencies(&input, &output);
        getErrorMessage(error);
        char captureDuration[160];
        if (!m_captureBufferMs) strcpy(captureDuration, "Windows default (CaptureBufferMs=0)");
        else if (!m_engine.hCapturePin) snprintf(captureDuration, sizeof(captureDuration), "%ld ms requested; device not open", m_captureBufferMs);
        else if (m_engine.captureBufferError)
            snprintf(captureDuration, sizeof(captureDuration), "%ld ms rejected (error %lu); Windows default retained",
                     m_captureBufferMs, (unsigned long)m_engine.captureBufferError);
        else snprintf(captureDuration, sizeof(captureDuration), "%ld ms accepted; %s", m_captureBufferMs,
                      AlignedCapture() ? "measured UMC22 input delay reported"
                                       : "default input delay reported (gain varies at this size)");
        long queued = m_buffersReady ? (long)m_engine.renderPackets - 1 : 1 + m_extraOutputBuffers;
        char alignment[160];
        if (stats.queuedInputSamples < 0) snprintf(alignment, sizeof(alignment), "not measured yet");
        else snprintf(alignment, sizeof(alignment), "%.2f ms queued at the minimum, %lld buffers trimmed, %ld kept in reserve",
                      stats.queuedInputSamples * millisecondsPerSample, stats.inputTrims, stats.inputReserve);
        char text[3072];
        snprintf(text, sizeof(text),
            "ASIO-for-UMC22 " DRIVER_VERSION_TEXT "\nDevice: %s (Windows driver: %s)\nDeviceName setting: %s\n"
            "%.0f Hz, %ld samples (%.2f ms per buffer)\n"
            "Output queue: %ld buffer(s) ahead, %ld extra\n"
            "USB capture request: %s\n"
            "Input alignment: %s\n"
            "Latency reported to the host: input %.2f ms, output %.2f ms\n"
            "Latency offsets in asio_settings.ini: input %ld us, output %ld us\n\n"
            "Callbacks: %lld\nOutput dropouts: %lld (output delayed %.1f ms in total)\n"
            "Input buffers skipped to stay in sync: %lld\nCallback budget overruns: %lld\n"
            "Longest callback: %.3f ms\nLongest callback interval: %.3f ms\n"
            "I/O errors: %lld\nWait timeouts: %lld\nShort capture packets: %lld\n"
            "Page lock failures: %ld\nLast error: %s\n\n"
            "Host: ASIO version %ld, time info %s, dropout reports %s, reset requests %s\n\n"
            "Measure round-trip latency with tools/latency_test and a loopback cable.",
            device, service, setting, m_sampleRate, m_bufferSize, m_bufferSize * millisecondsPerSample,
            queued, queued - 1, captureDuration, alignment,
            input * millisecondsPerSample, output * millisecondsPerSample, m_inputOffsetUs, m_outputOffsetUs,
            stats.callbacks, stats.dropouts, stats.slipSamples * millisecondsPerSample,
            stats.inputSkips, stats.callbackOverruns,
            stats.maxCallbackTicks * millisecondsPerTick, stats.maxCallbackGapTicks * millisecondsPerTick,
            stats.ioErrors, stats.waitTimeouts, stats.shortReads,
            stats.pageLockFailures + (m_hostMemory && !m_hostLocked ? 1L : 0L), error,
            m_hostVersion, m_timeInfoSupported ? "yes" : "no", m_overloadSupported ? "yes" : "no",
            m_resetSupported ? "yes" : "no");
        panel::Context context(text, m_sampleRate, m_preferredSize, m_extraOutputBuffers);
        long long problems = stats.ioErrors + stats.waitTimeouts + stats.shortReads + stats.pageLockFailures +
                             (m_hostMemory && !m_hostLocked ? 1 : 0);
        if (!m_engine.hCapturePin) {
            snprintf(context.summary, sizeof(context.summary), "Device not open");
            context.summaryTone = panel::kWarning;
        } else if (!stats.callbacks) {
            snprintf(context.summary, sizeof(context.summary), "Device open, not streaming yet");
        } else if (stats.dropouts || problems) {
            char dropouts[48] = "", errors[48] = "";
            if (stats.dropouts) snprintf(dropouts, sizeof(dropouts), "%lld output dropout%s", stats.dropouts,
                                         stats.dropouts == 1 ? "" : "s");
            if (problems) snprintf(errors, sizeof(errors), "%lld I/O problem%s", problems, problems == 1 ? "" : "s");
            snprintf(context.summary, sizeof(context.summary), "%s%s%s this session", dropouts,
                     stats.dropouts && problems ? " and " : "", errors);
            context.summaryTone = panel::kWarning;
        } else {
            snprintf(context.summary, sizeof(context.summary), "Streaming cleanly: no dropouts or errors this session");
            context.summaryTone = panel::kGood;
        }

        context.Add("Device", 0, "Device", m_engine.deviceName[0] ? panel::kPlain : panel::kWarning, "%s", device);
        context.Add(NULL, 0, "Windows driver", panel::kPlain, "%s", service);
        context.Add(NULL, 0, "DeviceName setting", panel::kPlain, "%s",
                    m_deviceName[0] ? setting : "Not set: first PCM2902 device");
        context.Add(NULL, 0, "Format", panel::kPlain, "%.0f Hz, %ld samples (%.2f ms)", m_sampleRate, m_bufferSize,
                    m_bufferSize * millisecondsPerSample);

        context.Add("Latency", 0, "Reported to host", panel::kPlain, "Input %.2f ms, output %.2f ms",
                    input * millisecondsPerSample, output * millisecondsPerSample);
        context.Add(NULL, 0, "Output queue", panel::kPlain, "%ld buffer%s ahead (%ld extra)", queued,
                    queued == 1 ? "" : "s", queued - 1);
        if (!m_captureBufferMs) context.Add(NULL, 0, "USB capture", panel::kPlain, "Windows default (10 ms blocks)");
        else if (!m_engine.hCapturePin) context.Add(NULL, 0, "USB capture", panel::kPlain, "%ld ms requested", m_captureBufferMs);
        else if (m_engine.captureBufferError)
            context.Add(NULL, 0, "USB capture", panel::kWarning, "%ld ms rejected (error %lu)", m_captureBufferMs,
                        (unsigned long)m_engine.captureBufferError);
        else if (AlignedCapture()) context.Add(NULL, 0, "USB capture", panel::kGood, "%ld ms blocks accepted", m_captureBufferMs);
        else context.Add(NULL, 0, "USB capture", panel::kPlain, "%ld ms accepted, default delay reported", m_captureBufferMs);
        if (stats.queuedInputSamples < 0) context.Add(NULL, 0, "Input alignment", panel::kPlain, "Not measured yet");
        else context.Add(NULL, 0, "Input alignment", panel::kPlain, "%.2f ms queued, %lld trimmed, reserve %ld",
                         stats.queuedInputSamples * millisecondsPerSample, stats.inputTrims, stats.inputReserve);
        if (!m_inputOffsetUs && !m_outputOffsetUs) context.Add(NULL, 0, "Calibration", panel::kPlain, "Built-in (no offsets)");
        else context.Add(NULL, 0, "Calibration", panel::kPlain, "Input %+ld us, output %+ld us", m_inputOffsetUs, m_outputOffsetUs);

        double bufferMs = m_bufferSize * millisecondsPerSample;
        double longest = stats.maxCallbackTicks * millisecondsPerTick;
        context.Add("This session", 1, "Callbacks", panel::kPlain, "%lld", stats.callbacks);
        if (stats.dropouts)
            context.Add(NULL, 1, "Output dropouts", panel::kWarning, "%lld (%.1f ms delayed in total)", stats.dropouts,
                        stats.slipSamples * millisecondsPerSample);
        else context.Add(NULL, 1, "Output dropouts", stats.callbacks ? panel::kGood : panel::kPlain, "None");
        context.Add(NULL, 1, "Input skips", stats.inputSkips ? panel::kWarning : panel::kPlain, "%lld", stats.inputSkips);
        context.Add(NULL, 1, "Budget overruns", stats.callbackOverruns ? panel::kWarning : panel::kPlain, "%lld",
                    stats.callbackOverruns);
        context.Add(NULL, 1, "Longest callback", longest > bufferMs ? panel::kWarning : panel::kPlain,
                    "%.3f ms (buffer is %.2f ms)", longest, bufferMs);
        context.Add(NULL, 1, "Longest interval", panel::kPlain, "%.3f ms", stats.maxCallbackGapTicks * millisecondsPerTick);
        if (problems)
            context.Add(NULL, 1, "I/O problems", panel::kWarning, "%lld errors, %lld timeouts, %lld short, %ld locks",
                        stats.ioErrors, stats.waitTimeouts, stats.shortReads,
                        stats.pageLockFailures + (m_hostMemory && !m_hostLocked ? 1L : 0L));
        else context.Add(NULL, 1, "I/O problems", stats.callbacks ? panel::kGood : panel::kPlain, "None");
        context.Add(NULL, 1, "Last error", strcmp(error, "No error") ? panel::kWarning : panel::kPlain, "%s", error);

        char features[96] = "";
        if (m_timeInfoSupported) strcat(features, "time info, ");
        if (m_overloadSupported) strcat(features, "dropout reports, ");
        if (m_resetSupported) strcat(features, "reset requests, ");
        if (features[0]) features[strlen(features) - 2] = 0;
        context.Add("Host", 1, "ASIO version", panel::kPlain, "%ld", m_hostVersion);
        context.Add(NULL, 1, "Supports", panel::kPlain, "%s", features[0] ? features : "No optional features");

        panel::Template dialog(L"ASIO-for-UMC22");
        panel::Build(&dialog, context);
        INT_PTR result = DialogBoxIndirectParamW(g_hModule, dialog.Data(), m_window, panel::Proc, (LPARAM)&context);
        if (result == -1) MessageBoxA(m_window, text, "ASIO-for-UMC22 diagnostics", MB_OK | MB_ICONINFORMATION);
        if (result != IDOK ||
            (context.bufferSize == m_preferredSize && context.extraOutputBuffers == m_extraOutputBuffers)) return ASE_OK;
        bool reopening = m_buffersReady && m_resetSupported;
        if (!ApplySettings(context.bufferSize, context.extraOutputBuffers))
            MessageBoxA(m_window, "The new settings could not be saved to asio_settings.ini, so they apply "
                        "only until the host closes the driver.", "ASIO-for-UMC22", MB_OK | MB_ICONWARNING);
        if (m_buffersReady && !reopening)
            MessageBoxA(m_window, "Reopen the driver in your host to use the new settings.", "ASIO-for-UMC22",
                        MB_OK | MB_ICONINFORMATION);
        return ASE_OK;
    }

    // Saves a buffer size and output safety setting to asio_settings.ini for the next
    // time buffers are created, and asks the host to reopen the driver if buffers
    // exist. Returns false if a value is invalid or could not be saved.
    bool ApplySettings(long size, long extraOutputBuffers) {
        if (!ValidBufferSize(size) || extraOutputBuffers < 0 || extraOutputBuffers > KS_MAX_EXTRA_OUTPUT_BUFFERS)
            return false;
        bool saved = m_settingsPath[0] != 0, changed = false;
        WCHAR value[16];
        if (size != m_preferredSize) {
            m_preferredSize = size;
            swprintf(value, 16, L"%ld", size);
            saved = saved && WritePrivateProfileStringW(L"Audio", L"BufferSize", value, m_settingsPath);
            changed = true;
        }
        if (extraOutputBuffers != m_extraOutputBuffers) {
            m_extraOutputBuffers = extraOutputBuffers;
            swprintf(value, 16, L"%ld", extraOutputBuffers);
            saved = saved && WritePrivateProfileStringW(L"Audio", L"ExtraOutputBuffers", value, m_settingsPath);
            changed = true;
        }
        if (changed && m_buffersReady && m_resetSupported) m_callbacks.asioMessage(kAsioResetRequest, 0, NULL, NULL);
        return saved;
    }
    bool SetPreferredBufferSize(long size) { return ApplySettings(size, m_extraOutputBuffers); }
    ASIOError future(long selector, void* params) override {
        if (selector == kAsioCanTimeInfo || selector == kAsioCanReportOverload) return ASE_SUCCESS;
        if (selector == kAsioGetInternalBufferSamples) {
            if (!params) return ASE_InvalidParameter;
            long input, output;
            getLatencies(&input, &output);
            ASIOInternalBufferInfo* info = static_cast<ASIOInternalBufferInfo*>(params);
            info->inputSamples = input > m_bufferSize ? input - m_bufferSize : 0;
            info->outputSamples = output > m_bufferSize ? output - m_bufferSize : 0;
            return ASE_SUCCESS;
        }
        return ASE_NotPresent; // The UMC22 has no input monitoring, gain, meter, time code or transport control.
    }
    ASIOError outputReady() override { return ASE_NotPresent; }

    void PrepareInput(long index, const void* input) {
        if (input) PcmToFloat((const int16_t*)input, m_hostInputBuffers[0][index], m_hostInputBuffers[1][index], m_bufferSize);
        else {
            memset(m_hostInputBuffers[0][index], 0, m_bufferSize * sizeof(float));
            memset(m_hostInputBuffers[1][index], 0, m_bufferSize * sizeof(float));
        }
    }

    void ProcessAudio(long index, void* output, ULONGLONG timestamp) {
        // Positions follow the device: output time lost in dropouts advances them too.
        // The engine updates these counters on this thread, before calling back.
        m_nextSamplePosition += m_engine.slipSamples - m_slipSeen;
        m_slipSeen = m_engine.slipSamples;
        if (m_engine.dropouts != m_dropoutsSeen) {
            m_dropoutsSeen = m_engine.dropouts;
            if (m_overloadSupported) m_callbacks.asioMessage(kAsioOverload, 0, NULL, NULL);
        }
        PublishPosition(m_nextSamplePosition, timestamp);
        if (m_timeInfoSupported) {
            ASIOTime time = {};
            time.timeInfo.speed = 1.0;
            time.timeInfo.sampleRate = m_sampleRate;
            time.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid | kSpeedValid;
            time.timeInfo.systemTime.hi = (unsigned long)(timestamp >> 32);
            time.timeInfo.systemTime.lo = (unsigned long)timestamp;
            time.timeInfo.samplePosition.hi = (unsigned long)(m_nextSamplePosition >> 32);
            time.timeInfo.samplePosition.lo = (unsigned long)m_nextSamplePosition;
            m_callbacks.bufferSwitchTimeInfo(&time, index, ASIOTrue);
        } else m_callbacks.bufferSwitch(index, ASIOTrue);
        if (output) FloatToPcm(m_active[1][0] ? m_hostOutputBuffers[0][index] : NULL,
                              m_active[1][1] ? m_hostOutputBuffers[1][index] : NULL,
                              (int16_t*)output, m_bufferSize);
        m_nextSamplePosition += m_bufferSize;
    }
    static void StaticInputReady(void* context, long index, const void* input) {
        static_cast<MiniAsioDriver*>(context)->PrepareInput(index, input);
    }
    static void StaticBufferReady(void* context, long index, void* output, ULONGLONG timestamp) {
        static_cast<MiniAsioDriver*>(context)->ProcessAudio(index, output, timestamp);
    }
    static void StaticError(void* context, DWORD error) {
        MiniAsioDriver* driver = static_cast<MiniAsioDriver*>(context);
        driver->m_error.store(error);
        if (driver->m_resetSupported) driver->m_callbacks.asioMessage(kAsioResetRequest, 0, NULL, NULL);
    }
};

class MiniClassFactory : public IClassFactory {
    LONG m_refCount;
public:
    MiniClassFactory() : m_refCount(1) { InterlockedIncrement(&g_objectCount); }
    virtual ~MiniClassFactory() { InterlockedDecrement(&g_objectCount); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG count = InterlockedDecrement(&m_refCount);
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (outer) return CLASS_E_NOAGGREGATION;
        MiniAsioDriver* driver = new (std::nothrow) MiniAsioDriver();
        if (!driver) return E_OUTOFMEMORY;
        HRESULT hr = driver->QueryInterface(riid, ppv);
        driver->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_serverLocks);
        else InterlockedDecrement(&g_serverLocks);
        return S_OK;
    }
};

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (!IsEqualCLSID(clsid, CLSID_AsioForUmc22)) return CLASS_E_CLASSNOTAVAILABLE;
    MiniClassFactory* factory = new (std::nothrow) MiniClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}
extern "C" HRESULT WINAPI DllCanUnloadNow(void) {
    return InterlockedCompareExchange(&g_objectCount, 0, 0) == 0 &&
           InterlockedCompareExchange(&g_serverLocks, 0, 0) == 0 ? S_OK : S_FALSE;
}

static const WCHAR* clsidKey = L"Software\\Classes\\CLSID\\{B2A0F51E-32E8-48D3-8902-6CD452D7EF31}";
static const WCHAR* serverKey = L"Software\\Classes\\CLSID\\{B2A0F51E-32E8-48D3-8902-6CD452D7EF31}\\InprocServer32";
static const WCHAR* asioKey = L"Software\\ASIO\\ASIO-for-UMC22";
static LONG WriteRegistryString(const WCHAR* path, const WCHAR* name, const WCHAR* value) {
    HKEY key;
    LONG result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL);
    if (result != ERROR_SUCCESS) return result;
    result = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value, (DWORD)((wcslen(value) + 1) * sizeof(WCHAR)));
    RegCloseKey(key);
    return result;
}
extern "C" HRESULT WINAPI DllRegisterServer(void) {
    WCHAR path[MAX_PATH];
    DWORD length = GetModuleFileNameW(g_hModule, path, MAX_PATH);
    if (!length) return HRESULT_FROM_WIN32(GetLastError());
    if (length >= MAX_PATH) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    LONG error = WriteRegistryString(serverKey, NULL, path);
    if (!error) error = WriteRegistryString(serverKey, L"ThreadingModel", L"Both");
    if (!error) error = WriteRegistryString(asioKey, L"CLSID", L"{B2A0F51E-32E8-48D3-8902-6CD452D7EF31}");
    if (!error) error = WriteRegistryString(asioKey, L"Description", L"ASIO-for-UMC22");
    return HRESULT_FROM_WIN32(error);
}
extern "C" HRESULT WINAPI DllUnregisterServer(void) {
    const WCHAR* keys[] = {serverKey, clsidKey, asioKey};
    LONG firstError = ERROR_SUCCESS;
    for (const WCHAR* key : keys) {
        LONG result = RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
        if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND && !firstError) firstError = result;
    }
    return HRESULT_FROM_WIN32(firstError);
}
