#ifndef ASIO_TYPES_H
#define ASIO_TYPES_H

#include <windows.h>

typedef long ASIOBool;
enum {
    ASIOFalse = 0,
    ASIOTrue = 1
};

typedef long ASIOError;
enum {
    ASE_OK = 0,
    ASE_SUCCESS = 0x3f4847a0,
    ASE_NotPresent = -1000,
    ASE_HWMalfunction,
    ASE_InvalidParameter,
    ASE_InvalidMode,
    ASE_SPNotAdvancing,
    ASE_NoClock,
    ASE_NoMemory
};

typedef long ASIOSampleType;
enum {
    ASIOSTInt16MSB = 0,
    ASIOSTInt24MSB = 1,
    ASIOSTInt32MSB = 2,
    ASIOSTFloat32MSB = 3,
    ASIOSTFloat64MSB = 4,
    ASIOSTInt32MSB16 = 8,
    ASIOSTInt32MSB18 = 9,
    ASIOSTInt32MSB20 = 10,
    ASIOSTInt32MSB24 = 11,
    ASIOSTInt16LSB = 16,
    ASIOSTInt24LSB = 17,
    ASIOSTInt32LSB = 18,
    ASIOSTFloat32LSB = 19,
    ASIOSTFloat64LSB = 20,
    ASIOSTInt32LSB16 = 24,
    ASIOSTInt32LSB18 = 25,
    ASIOSTInt32LSB20 = 26,
    ASIOSTInt32LSB24 = 27
};

typedef double ASIOSampleRate;

// ASIO uses four-byte structure packing, including on 64-bit Windows.
#pragma pack(push, 4)

typedef struct ASIOSamples {
    unsigned long hi;
    unsigned long lo;
} ASIOSamples;

typedef struct ASIOTimeStamp {
    unsigned long hi;
    unsigned long lo;
} ASIOTimeStamp;

typedef struct ASIOTimeCode {
    double speed;
    ASIOSamples timeCodeSamples;
    unsigned long flags;
    char future[64];
} ASIOTimeCode;

typedef struct ASIOTimeInfo {
    double speed;
    ASIOTimeStamp systemTime;
    ASIOSamples samplePosition;
    ASIOSampleRate sampleRate;
    unsigned long flags;
    char reserved[12];
} ASIOTimeInfo;

typedef struct ASIOTime {
    long reserved[4];
    ASIOTimeInfo timeInfo;
    ASIOTimeCode timeCode;
} ASIOTime;

typedef struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
} ASIOCallbacks;

typedef struct ASIOBufferInfo {
    ASIOBool isInput;
    long channelNum;
    void* buffers[2];
} ASIOBufferInfo;

typedef struct ASIOChannelInfo {
    long channel;
    ASIOBool isInput;
    ASIOBool isActive;
    long channelGroup;
    ASIOSampleType type;
    char name[32];
} ASIOChannelInfo;

typedef struct ASIOClockSource {
    long index;
    long associatedChannel;
    long associatedGroup;
    ASIOBool isCurrentSource;
    char name[32];
} ASIOClockSource;

// Driver buffering beyond the ASIO buffers, already included in getLatencies().
typedef struct ASIOInternalBufferInfo {
    long inputSamples;
    long outputSamples;
} ASIOInternalBufferInfo;

#pragma pack(pop)

// ASIOTimeInfo flags.
enum {
    kSystemTimeValid = 1,
    kSamplePositionValid = 2,
    kSampleRateValid = 4,
    kSpeedValid = 8
};

// asioMessage() selectors, sent by the driver to the host.
enum {
    kAsioSelectorSupported = 1,
    kAsioEngineVersion = 2,
    kAsioResetRequest = 3,
    kAsioSupportsTimeInfo = 7,
    kAsioOverload = 15 // ASIO 2.3: the driver detected a dropout.
};

// future() selectors, sent by the host to the driver.
enum {
    kAsioCanInputMonitor = 9,
    kAsioCanTimeInfo = 10,
    kAsioCanReportOverload = 0x24042012,       // ASIO 2.3
    kAsioGetInternalBufferSamples = 0x25042012 // ASIO 2.3: ASIOInternalBufferInfo* in params.
};

#ifdef __cplusplus
static_assert(sizeof(ASIOTimeInfo) == 48, "ASIOTimeInfo ABI mismatch");
static_assert(sizeof(ASIOTimeCode) == 84, "ASIOTimeCode ABI mismatch");
static_assert(sizeof(ASIOTime) == 148, "ASIOTime ABI mismatch");
#endif

// Steinberg ASIO COM Interface GUID
// {763D10A0-508F-11CF-8D75-00A0C90391D3}
static const GUID IID_IASIO = {
    0x763D10A0, 0x508F, 0x11CF, { 0x8D, 0x75, 0x00, 0xA0, 0xC9, 0x03, 0x91, 0xD3 }
};

#ifdef __cplusplus
struct IASIO : public IUnknown {
    virtual ASIOBool init(void *sysHandle) = 0;
    virtual void getDriverName(char *name) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char *string) = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long *numInputChannels, long *numOutputChannels) = 0;
    virtual ASIOError getLatencies(long *inputLatency, long *outputLatency) = 0;
    virtual ASIOError getBufferSize(long *minSize, long *maxSize, long *preferredSize, long *granularity) = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate *sampleRate) = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getClockSources(ASIOClockSource *clocks, long *numSources) = 0;
    virtual ASIOError setClockSource(long reference) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo *info) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize, ASIOCallbacks *callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(long selector, void *opt) = 0;
    virtual ASIOError outputReady() = 0;
};
#endif

#endif // ASIO_TYPES_H
