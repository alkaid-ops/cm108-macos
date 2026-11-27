/*
 * Ranhe RH-899X AudioServerPlugIn driver.
 *
 * CoreAudio HAL output device -> lock-free SPSC ring -> USB worker ->
 * High-Speed isochronous endpooint 0x02 (64 x 24-byte microframes per
 * 8 ms transfer).
 *
 * The driver does not depend on BlackHole. It is a normal AudioServerPlugIn
 * and must be code signed by an Apple-issued certificate to load on macOS.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>
#include <math.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define kDeviceID 0x100u
#define kStreamID 0x101u
#define kDefaultRate 48000u
#define kBitstreamRate 48000u
#define kChannels 2u
#define kRingFrames 65536u
#define kRingMask (kRingFrames - 1)

static const UInt32 kSupportedRates[] = { 48000u, 96000u, 192000u };
#define kSupportedRateCount (sizeof(kSupportedRates) / sizeof(kSupportedRates[0]))
/* 96/192 kHz stay accepted by SetPropertyData for diagnostics, but are not
 * advertised: both caused the RH-899X firmware to stop responding on real
 * hardware. Only 48 kHz is exposed to CoreAudio. */
#define kAdvertisedRateCount 1u

/* One IOUSBIsocFrame element is one 125 us microframe. The packet size is
 * rate dependent: 6/12/24 stereo frames at 48/96/192 kHz. */
#define kMicroframes 64u
#define kBitstreamMicroframes 8u   /* 8 microframes = 1 ms per transfer */
#define kSlots 8
#define kMaxAhead 32

/* Keep the ring at the same *time* depth (about 21 ms) at every rate. */
#define kLevelFrames(transferFrames, n) ((UInt64)(transferFrames) * (n))

/* Tuning knobs used by tools/autotune.py. Crank them with -D on the compile
 * line; the defaults below are the shipped values. */
#ifndef RH899X_BYTES_PER_FRAME
#define RH899X_BYTES_PER_FRAME 0u
#endif
#ifndef RH899X_BUFFER_FRAMES
#define RH899X_BUFFER_FRAMES 512u
#endif
#ifndef RH899X_LATENCY_FRAMES
#define RH899X_LATENCY_FRAMES 0u
#endif
#ifndef RH899X_SAFETY_FRAMES
#define RH899X_SAFETY_FRAMES 0u
#endif

#define kRanheStatsSelector 'rhst'

enum {
    kFormatFloat32 = 0,
    kFormatPcm16NonMixable = 1,
    kFormat60958AC3 = 2
};

typedef struct RanheStats {
    UInt64 usbTransfers;
    UInt64 usbFrames;
    UInt64 usbErrors;
    UInt64 frameErrors;
    UInt64 underruns;
    UInt64 overruns;
    UInt64 drops;
    UInt64 dups;
    UInt32 ringLevel;
    UInt32 ioClients;
} RanheStats;

typedef struct XferSlot {
    struct Driver *driver;
    unsigned char *buf;
    IOUSBIsocFrame *frames;
    atomic_int busy;
    UInt64 startFrame;
    UInt32 numFrames;
    UInt32 bufferBytes;
} XferSlot;

typedef struct Driver {
    AudioServerPlugInDriverInterface *vtable;
    UInt32 refCount;
    AudioServerPlugInHostRef host;

    pthread_mutex_t lifecycleLock;
    atomic_bool running;
    atomic_bool threadStarted;
    pthread_t thread;
    UInt32 ioClients;

    UInt32 bufferFrames;
    _Atomic UInt32 rate;
    atomic_int formatMode;
    atomic_int supportsMixing;
    atomic_bool muted;
    _Atomic float volume;

    Float32 *ring;
    atomic_ullong head;
    atomic_ullong tail;
    int16_t *bitstreamRing;
    atomic_ullong bitstreamHead;
    atomic_ullong bitstreamTail;
    atomic_ullong usbTransfers;
    atomic_ullong usbFrames;
    atomic_ullong usbErrors;
    atomic_ullong frameErrors;
    atomic_ullong underruns;
    atomic_ullong overruns;
    atomic_ullong drops;
    atomic_ullong dups;

    atomic_ullong startHost;
    atomic_ullong seed;
} Driver;

static mach_timebase_info_data_t gTimebase;
static os_log_t gHalLog;
static CFStringRef kUID = CFSTR("RH899X-0c1c-3a1d");
static CFStringRef kModelUID = CFSTR("com.ranhe.rh899x.model");
static CFStringRef kDeviceName = CFSTR("RH-899X");
static CFStringRef kManufacturer = CFSTR("Ranhe");

static void halLog(const char *fmt, ...) {
    if (!gHalLog) gHalLog = os_log_create("com.ranhe.rh899x.audio", "hal");
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    os_log_info(gHalLog, "%{public}s", buf);
}

static HRESULT QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface);
static ULONG AddRef(void *inDriver);
static ULONG Release(void *inDriver);
static OSStatus Initialize(AudioServerPlugInDriverRef inDriver,
                           AudioServerPlugInHostRef inHost);
static OSStatus CreateDevice(AudioServerPlugInDriverRef inDriver,
                             CFDictionaryRef inDescription,
                             const AudioServerPlugInClientInfo *inClientInfo,
                             AudioObjectID *outDeviceObjectID);
static OSStatus DestroyDevice(AudioServerPlugInDriverRef inDriver,
                              AudioObjectID inDeviceObjectID);
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef inDriver,
                                AudioObjectID inDeviceObjectID,
                                const AudioServerPlugInClientInfo *inClientInfo);
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef inDriver,
                                   AudioObjectID inDeviceObjectID,
                                   const AudioServerPlugInClientInfo *inClientInfo);
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                 AudioObjectID inDeviceObjectID,
                                                 UInt64 inChangeAction, void *inChangeInfo);
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                               AudioObjectID inDeviceObjectID,
                                               UInt64 inChangeAction, void *inChangeInfo);
static Boolean HasProperty(AudioServerPlugInDriverRef inDriver,
                           AudioObjectID inObjectID, pid_t inClientProcessID,
                           const AudioObjectPropertyAddress *inAddress);
static OSStatus IsPropertySettable(AudioServerPlugInDriverRef inDriver,
                                   AudioObjectID inObjectID, pid_t inClientProcessID,
                                   const AudioObjectPropertyAddress *inAddress,
                                   Boolean *outIsSettable);
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef inDriver,
                                    AudioObjectID inObjectID, pid_t inClientProcessID,
                                    const AudioObjectPropertyAddress *inAddress,
                                    UInt32 inQualifierDataSize,
                                    const void *inQualifierData, UInt32 *outDataSize);
static OSStatus GetPropertyData(AudioServerPlugInDriverRef inDriver,
                                AudioObjectID inObjectID, pid_t inClientProcessID,
                                const AudioObjectPropertyAddress *inAddress,
                                UInt32 inQualifierDataSize,
                                const void *inQualifierData,
                                UInt32 inDataSize, UInt32 *outDataSize,
                                void *outData);
static OSStatus SetPropertyData(AudioServerPlugInDriverRef inDriver,
                                AudioObjectID inObjectID, pid_t inClientProcessID,
                                const AudioObjectPropertyAddress *inAddress,
                                UInt32 inQualifierDataSize,
                                const void *inQualifierData, UInt32 inDataSize,
                                const void *inData);
static OSStatus StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                        UInt32 inClientID);
static OSStatus StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                       UInt32 inClientID);
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver,
                                 AudioObjectID inDeviceObjectID, UInt32 inClientID,
                                 Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed);
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef inDriver,
                                  AudioObjectID inDeviceObjectID, UInt32 inClientID,
                                  UInt32 inOperationID, Boolean *outWillDo,
                                  Boolean *outWillDoInPlace);
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef inDriver,
                                 AudioObjectID inDeviceObjectID, UInt32 inClientID,
                                 UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                 const AudioServerPlugInIOCycleInfo *inIOCycleInfo);
static OSStatus DoIOOperation(AudioServerPlugInDriverRef inDriver,
                              AudioObjectID inDeviceObjectID,
                              AudioObjectID inStreamObjectID, UInt32 inClientID,
                              UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                              const AudioServerPlugInIOCycleInfo *inIOCycleInfo,
                              void *ioMainBuffer, void *ioSecondaryBuffer);
static OSStatus EndIOOperation(AudioServerPlugInDriverRef inDriver,
                               AudioObjectID inDeviceObjectID, UInt32 inClientID,
                               UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                               const AudioServerPlugInIOCycleInfo *inIOCycleInfo);

static void *usbThread(void *arg);
static void notifyProperty(Driver *d, AudioObjectID obj,
                           AudioObjectPropertySelector sel,
                           AudioObjectPropertyScope scope,
                           AudioObjectPropertyElement element);

static AudioServerPlugInDriverInterface gVTable = {
    NULL,
    QueryInterface,
    AddRef,
    Release,
    Initialize,
    CreateDevice,
    DestroyDevice,
    AddDeviceClient,
    RemoveDeviceClient,
    PerformDeviceConfigurationChange,
    AbortDeviceConfigurationChange,
    HasProperty,
    IsPropertySettable,
    GetPropertyDataSize,
    GetPropertyData,
    SetPropertyData,
    StartIO,
    StopIO,
    GetZeroTimeStamp,
    WillDoIOOperation,
    BeginIOOperation,
    DoIOOperation,
    EndIOOperation
};

static UInt64 hostTicksToNanos(UInt64 ticks) {
    return (UInt64)((double)ticks * gTimebase.numer / gTimebase.denom);
}

static UInt64 nanosToHostTicks(UInt64 ns) {
    return (UInt64)((double)ns * gTimebase.denom / gTimebase.numer);
}

static UInt32 samplesPerMicroframeForRate(UInt32 rate) {
    return rate / 8000u;
}

static UInt32 transferFramesForRate(UInt32 rate) {
    return kMicroframes * samplesPerMicroframeForRate(rate);
}

static UInt32 transferBytesForRate(UInt32 rate) {
    return transferFramesForRate(rate) * 4u;
}

static UInt32 microframeBytesForRate(UInt32 rate) {
    return samplesPerMicroframeForRate(rate) * 4u;
}

static Boolean rateIsSupported(Float64 rate) {
    if (rate != (Float64)(UInt32)rate) return false;
    for (UInt32 i = 0; i < kSupportedRateCount; i++)
        if ((UInt32)rate == kSupportedRates[i]) return true;
    return false;
}

static void setASBDForMode(AudioStreamBasicDescription *asbd, int mode, UInt32 rate) {
    memset(asbd, 0, sizeof(*asbd));
    asbd->mSampleRate = rate;
    if (mode == kFormat60958AC3) {
        asbd->mFormatID = kAudioFormat60958AC3;
        asbd->mFormatFlags = kAudioFormatFlagIsSignedInteger |
                             kAudioFormatFlagIsPacked |
                             kAudioFormatFlagIsNonMixable;
    } else if (mode == kFormatPcm16NonMixable) {
        asbd->mFormatID = kAudioFormatLinearPCM;
        asbd->mFormatFlags = kAudioFormatFlagIsSignedInteger |
                             kAudioFormatFlagIsPacked |
                             kAudioFormatFlagIsNonMixable;
    } else {
        asbd->mFormatID = kAudioFormatLinearPCM;
        asbd->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    }
    if (mode == kFormatFloat32) {
        asbd->mBytesPerPacket = 4 * kChannels;
        asbd->mFramesPerPacket = 1;
        asbd->mBytesPerFrame = 4 * kChannels;
    } else {
        asbd->mBytesPerPacket = 6144;
        asbd->mFramesPerPacket = 1536;
        asbd->mBytesPerFrame = RH899X_BYTES_PER_FRAME;
    }
    asbd->mChannelsPerFrame = kChannels;
    asbd->mBitsPerChannel = mode == kFormatFloat32 ? 32 : 16;
}

static void setASBD(Driver *d, AudioStreamBasicDescription *asbd) {
    UInt32 rate = atomic_load(&d->rate);
    int mode = atomic_load(&d->formatMode);
    if (mode == kFormat60958AC3) rate = kBitstreamRate;
    setASBDForMode(asbd, mode, rate);
}

static Boolean isBitstream(Driver *d) {
    return atomic_load(&d->formatMode) != kFormatFloat32;
}

/* ---------------- lock-free SPSC ring ---------------- */

static UInt64 ringLevel(Driver *d) {
    UInt64 head = atomic_load_explicit(&d->head, memory_order_acquire);
    UInt64 tail = atomic_load_explicit(&d->tail, memory_order_acquire);
    return head - tail;
}

static UInt64 bitstreamLevel(Driver *d) {
    UInt64 head = atomic_load_explicit(&d->bitstreamHead, memory_order_acquire);
    UInt64 tail = atomic_load_explicit(&d->bitstreamTail, memory_order_acquire);
    return head - tail;
}

static void ringPush(Driver *d, const Float32 *src, UInt32 frames) {
    UInt64 head = atomic_load_explicit(&d->head, memory_order_relaxed);
    UInt64 tail = atomic_load_explicit(&d->tail, memory_order_acquire);
    UInt64 used = head - tail;
    UInt64 freeFrames = kRingFrames - used;
    UInt32 n = frames < freeFrames ? frames : (UInt32)freeFrames;
    if (n < frames) atomic_fetch_add(&d->overruns, frames - n);
    for (UInt32 i = 0; i < n; i++) {
        UInt64 idx = (head + i) & kRingMask;
        d->ring[idx * kChannels] = src[i * kChannels];
        d->ring[idx * kChannels + 1] = src[i * kChannels + 1];
    }
    atomic_store_explicit(&d->head, head + n, memory_order_release);
}

static UInt32 ringPop(Driver *d, Float32 *dst, UInt32 frames) {
    UInt64 tail = atomic_load_explicit(&d->tail, memory_order_relaxed);
    UInt64 head = atomic_load_explicit(&d->head, memory_order_acquire);
    UInt64 used = head - tail;
    UInt32 n = frames < used ? frames : (UInt32)used;
    for (UInt32 i = 0; i < n; i++) {
        UInt64 idx = (tail + i) & kRingMask;
        dst[i * kChannels] = d->ring[idx * kChannels];
        dst[i * kChannels + 1] = d->ring[idx * kChannels + 1];
    }
    if (n < frames) atomic_fetch_add(&d->underruns, frames - n);
    atomic_store_explicit(&d->tail, tail + n, memory_order_release);
    return n;
}

static void bitstreamPush(Driver *d, const int16_t *src, UInt32 frames) {
    UInt64 head = atomic_load_explicit(&d->bitstreamHead, memory_order_relaxed);
    UInt64 tail = atomic_load_explicit(&d->bitstreamTail, memory_order_acquire);
    UInt64 used = head - tail;
    UInt64 freeFrames = kRingFrames - used;
    UInt32 n = frames < freeFrames ? frames : (UInt32)freeFrames;
    if (n < frames) atomic_fetch_add(&d->overruns, frames - n);
    for (UInt32 i = 0; i < n; i++) {
        UInt64 idx = (head + i) & kRingMask;
        d->bitstreamRing[idx * kChannels] = src[i * kChannels];
        d->bitstreamRing[idx * kChannels + 1] = src[i * kChannels + 1];
    }
    atomic_store_explicit(&d->bitstreamHead, head + n, memory_order_release);
}

static UInt32 bitstreamPop(Driver *d, int16_t *dst, UInt32 frames) {
    UInt64 tail = atomic_load_explicit(&d->bitstreamTail, memory_order_relaxed);
    UInt64 head = atomic_load_explicit(&d->bitstreamHead, memory_order_acquire);
    UInt64 used = head - tail;
    UInt32 n = frames < used ? frames : (UInt32)used;
    for (UInt32 i = 0; i < n; i++) {
        UInt64 idx = (tail + i) & kRingMask;
        dst[i * kChannels] = d->bitstreamRing[idx * kChannels];
        dst[i * kChannels + 1] = d->bitstreamRing[idx * kChannels + 1];
    }
    if (n < frames) atomic_fetch_add(&d->underruns, frames - n);
    atomic_store_explicit(&d->bitstreamTail, tail + n, memory_order_release);
    return n;
}

/* ---------------- USB worker ---------------- */

typedef struct UsbContext {
    io_service_t service;
    IOUSBDeviceInterface **dev;
    IOUSBInterfaceInterface **itf;
} UsbContext;

static void usbDone(void *refcon, IOReturn result, void *arg0) {
    XferSlot *slot = (XferSlot *)refcon;
    Driver *d = slot->driver;
    IOUSBIsocFrame *frames = (IOUSBIsocFrame *)arg0;
    UInt64 actBytes = 0;
    UInt64 frameErrors = 0;
    for (UInt32 m = 0; m < slot->numFrames; m++) {
        actBytes += frames[m].frActCount;
        if (frames[m].frStatus) frameErrors++;
    }
    atomic_fetch_add(&d->usbTransfers, 1);
    atomic_fetch_add(&d->usbFrames, actBytes / 4);
    atomic_fetch_add(&d->frameErrors, frameErrors);
    if (result != kIOReturnSuccess) atomic_fetch_add(&d->usbErrors, 1);
    atomic_store(&slot->busy, 0);
}

static int usbFind(UsbContext *ctx) {
    io_iterator_t iter = 0;
    IOServiceGetMatchingServices(kIOMainPortDefault,
                                 IOServiceMatching("IOUSBHostDevice"), &iter);
    io_service_t s, device = 0;
    while ((s = IOIteratorNext(iter))) {
        CFTypeRef vv = IORegistryEntryCreateCFProperty(s, CFSTR("idVendor"),
                                                       kCFAllocatorDefault, 0);
        CFTypeRef vp = IORegistryEntryCreateCFProperty(s, CFSTR("idProduct"),
                                                       kCFAllocatorDefault, 0);
        int vid = 0, pid = 0;
        if (vv) CFNumberGetValue((CFNumberRef)vv, kCFNumberIntType, &vid);
        if (vp) CFNumberGetValue((CFNumberRef)vp, kCFNumberIntType, &pid);
        if (vv) CFRelease(vv);
        if (vp) CFRelease(vp);
        if (vid == 0x0c1c && pid == 0x3a1d) { device = s; break; }
        IOObjectRelease(s);
    }
    IOObjectRelease(iter);
    if (!device) return 0;

    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    IOCreatePlugInInterfaceForService(device, kIOUSBDeviceUserClientTypeID,
                                      kIOCFPlugInInterfaceID, &plugin, &score);
    if (!plugin) { IOObjectRelease(device); return 0; }
    IOReturn r = (*plugin)->QueryInterface(plugin,
                CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&ctx->dev);
    (*plugin)->Release(plugin);
    if (r || !ctx->dev) { IOObjectRelease(device); return 0; }
    r = (*ctx->dev)->USBDeviceOpen(ctx->dev);
    if (r == kIOReturnExclusiveAccess) r = (*ctx->dev)->USBDeviceOpenSeize(ctx->dev);
    if (r) {
        (*ctx->dev)->Release(ctx->dev);
        ctx->dev = NULL;
        IOObjectRelease(device);
        return 0;
    }

    io_iterator_t itfIter = 0;
    IOUSBFindInterfaceRequest req = {kIOUSBFindInterfaceDontCare,
                                     kIOUSBFindInterfaceDontCare,
                                     kIOUSBFindInterfaceDontCare,
                                     kIOUSBFindInterfaceDontCare};
    (*ctx->dev)->CreateInterfaceIterator(ctx->dev, &req, &itfIter);
    io_service_t is;
    while ((is = IOIteratorNext(itfIter))) {
        CFTypeRef vn = IORegistryEntryCreateCFProperty(is, CFSTR("bInterfaceNumber"),
                                                       kCFAllocatorDefault, 0);
        int num = -1;
        if (vn) CFNumberGetValue((CFNumberRef)vn, kCFNumberIntType, &num);
        if (vn) CFRelease(vn);
        if (num == 1) {
            IOCFPlugInInterface **ip = NULL;
            IOCreatePlugInInterfaceForService(is, kIOUSBInterfaceUserClientTypeID,
                                              kIOCFPlugInInterfaceID, &ip, &score);
            IOObjectRelease(is);
            if (ip) {
                (*ip)->QueryInterface(ip,
                    CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID), (LPVOID *)&ctx->itf);
                (*ip)->Release(ip);
            }
            break;
        }
        IOObjectRelease(is);
    }
    IOObjectRelease(itfIter);
    ctx->service = device;
    if (!ctx->itf) {
        (*ctx->dev)->USBDeviceClose(ctx->dev);
        (*ctx->dev)->Release(ctx->dev);
        ctx->dev = NULL;
        IOObjectRelease(device);
        return 0;
    }
    return 1;
}

static void usbClose(UsbContext *ctx) {
    if (ctx->itf) {
        (*ctx->itf)->SetAlternateInterface(ctx->itf, 0);
        (*ctx->itf)->USBInterfaceClose(ctx->itf);
        (*ctx->itf)->Release(ctx->itf);
        ctx->itf = NULL;
    }
    if (ctx->dev) {
        (*ctx->dev)->USBDeviceClose(ctx->dev);
        (*ctx->dev)->Release(ctx->dev);
        ctx->dev = NULL;
    }
    if (ctx->service) {
        IOObjectRelease(ctx->service);
        ctx->service = 0;
    }
}

/* ---------------- lifecycle ---------------- */

static OSStatus Initialize(AudioServerPlugInDriverRef inDriver,
                           AudioServerPlugInHostRef inHost) {
    Driver *d = (Driver *)inDriver;
    halLog("Initialize host=%p", (void *)inHost);
    d->host = inHost;
    mach_timebase_info(&gTimebase);
    if (!d->ring) {
        d->ring = calloc(kRingFrames * kChannels, sizeof(Float32));
        if (!d->ring) return kAudioHardwareUnspecifiedError;
    }
    if (!d->bitstreamRing) {
        d->bitstreamRing = calloc(kRingFrames * kChannels, sizeof(int16_t));
        if (!d->bitstreamRing) return kAudioHardwareUnspecifiedError;
    }
    return noErr;
}

static OSStatus CreateDevice(AudioServerPlugInDriverRef inDriver,
                             CFDictionaryRef inDescription,
                             const AudioServerPlugInClientInfo *inClientInfo,
                             AudioObjectID *outDeviceObjectID) {
    (void)inDriver;
    (void)inClientInfo;
    halLog("CreateDevice desc=%p", (void *)inDescription);
    if (!outDeviceObjectID) return kAudio_ParamError;
    if (inDescription) {
        CFTypeRef uid = CFDictionaryGetValue(inDescription, CFSTR("UID"));
        if (uid && CFGetTypeID(uid) == CFStringGetTypeID() &&
            !CFEqual((CFStringRef)uid, kUID))
            return kAudioHardwareBadObjectError;
    }
    *outDeviceObjectID = kDeviceID;
    return noErr;
}

static void stopWorker(Driver *d) {
    if (atomic_load(&d->threadStarted)) {
        atomic_store(&d->running, false);
        pthread_join(d->thread, NULL);
        atomic_store(&d->threadStarted, false);
    }
    atomic_store(&d->running, false);
}

static OSStatus DestroyDevice(AudioServerPlugInDriverRef inDriver,
                              AudioObjectID inDeviceObjectID) {
    (void)inDeviceObjectID;
    stopWorker((Driver *)inDriver);
    return noErr;
}

static OSStatus AddDeviceClient(AudioServerPlugInDriverRef inDriver,
                                AudioObjectID inDeviceObjectID,
                                const AudioServerPlugInClientInfo *inClientInfo) {
    (void)inDriver;
    (void)inDeviceObjectID;
    (void)inClientInfo;
    return noErr;
}

static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef inDriver,
                                   AudioObjectID inDeviceObjectID,
                                   const AudioServerPlugInClientInfo *inClientInfo) {
    (void)inDriver;
    (void)inDeviceObjectID;
    (void)inClientInfo;
    return noErr;
}

static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                 AudioObjectID inDeviceObjectID,
                                                 UInt64 inChangeAction, void *inChangeInfo) {
    (void)inDriver;
    (void)inDeviceObjectID;
    (void)inChangeAction;
    (void)inChangeInfo;
    return noErr;
}

static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                               AudioObjectID inDeviceObjectID,
                                               UInt64 inChangeAction, void *inChangeInfo) {
    (void)inDriver;
    (void)inDeviceObjectID;
    (void)inChangeAction;
    (void)inChangeInfo;
    return noErr;
}

static OSStatus StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                        UInt32 inClientID) {
    Driver *d = (Driver *)inDriver;
    halLog("StartIO client=%u", inClientID);
    (void)inDeviceObjectID;
    (void)inClientID;
    pthread_mutex_lock(&d->lifecycleLock);
    if (d->ioClients == 0) {
        atomic_store(&d->head, 0);
        atomic_store(&d->tail, 0);
        atomic_store(&d->bitstreamHead, 0);
        atomic_store(&d->bitstreamTail, 0);
        atomic_store(&d->usbTransfers, 0);
        atomic_store(&d->usbFrames, 0);
        atomic_store(&d->usbErrors, 0);
        atomic_store(&d->frameErrors, 0);
        atomic_store(&d->underruns, 0);
        atomic_store(&d->overruns, 0);
        atomic_store(&d->drops, 0);
        atomic_store(&d->dups, 0);
        atomic_store(&d->startHost, mach_absolute_time());
        atomic_fetch_add(&d->seed, 1);
        atomic_store(&d->running, true);
        if (pthread_create(&d->thread, NULL, usbThread, d) == 0)
            atomic_store(&d->threadStarted, true);
        else
            atomic_store(&d->running, false);
    }
    d->ioClients++;
    pthread_mutex_unlock(&d->lifecycleLock);
    notifyProperty(d, kDeviceID, kAudioDevicePropertyDeviceIsRunning,
                   kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain);
    notifyProperty(d, kDeviceID, kAudioDevicePropertyDeviceIsRunningSomewhere,
                   kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain);
    return noErr;
}

static OSStatus StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                       UInt32 inClientID) {
    Driver *d = (Driver *)inDriver;
    halLog("StopIO client=%u mode=%d tx=%llu frames=%llu usb_err=%llu frame_err=%llu "
           "under=%llu over=%llu drops=%llu dups=%llu ring=%llu",
           inClientID,
           atomic_load(&d->formatMode),
           (unsigned long long)atomic_load(&d->usbTransfers),
           (unsigned long long)atomic_load(&d->usbFrames),
           (unsigned long long)atomic_load(&d->usbErrors),
           (unsigned long long)atomic_load(&d->frameErrors),
           (unsigned long long)atomic_load(&d->underruns),
           (unsigned long long)atomic_load(&d->overruns),
           (unsigned long long)atomic_load(&d->drops),
           (unsigned long long)atomic_load(&d->dups),
           (unsigned long long)(isBitstream(d) ? bitstreamLevel(d) : ringLevel(d)));
    (void)inDeviceObjectID;
    (void)inClientID;
    pthread_mutex_lock(&d->lifecycleLock);
    if (d->ioClients > 0) d->ioClients--;
    bool stop = d->ioClients == 0;
    pthread_mutex_unlock(&d->lifecycleLock);
    if (stop) stopWorker(d);
    notifyProperty(d, kDeviceID, kAudioDevicePropertyDeviceIsRunning,
                   kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain);
    notifyProperty(d, kDeviceID, kAudioDevicePropertyDeviceIsRunningSomewhere,
                   kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain);
    return noErr;
}

static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver,
                                 AudioObjectID inDeviceObjectID, UInt32 inClientID,
                                 Float64 *outSampleTime, UInt64 *outHostTime,
                                 UInt64 *outSeed) {
    Driver *d = (Driver *)inDriver;
    (void)inDeviceObjectID;
    (void)inClientID;
    if (!outSampleTime || !outHostTime || !outSeed) return kAudio_ParamError;
    UInt64 startHost = atomic_load(&d->startHost);
    UInt64 now = mach_absolute_time();
    Float64 period = d->bufferFrames ? d->bufferFrames : 512;
    Float64 elapsed = 0;
    if (startHost && now > startHost)
        elapsed = (Float64)hostTicksToNanos(now - startHost) / 1e9 *
                  atomic_load(&d->rate);
    Float64 stamp = floor(elapsed / period) * period;
    *outSampleTime = stamp;
    UInt64 ns = (UInt64)(stamp / atomic_load(&d->rate) * 1e9);
    *outHostTime = startHost ? startHost + nanosToHostTicks(ns) : now;
    *outSeed = atomic_load(&d->seed);
    return noErr;
}

static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef inDriver,
                                  AudioObjectID inDeviceObjectID, UInt32 inClientID,
                                  UInt32 inOperationID, Boolean *outWillDo,
                                  Boolean *outWillDoInPlace) {
    (void)inDriver;
    (void)inDeviceObjectID;
    (void)inClientID;
    if (!outWillDo || !outWillDoInPlace) return kAudio_ParamError;
    *outWillDo = inOperationID == kAudioServerPlugInIOOperationWriteMix;
    *outWillDoInPlace = true;
    return noErr;
}

static OSStatus BeginIOOperation(AudioServerPlugInDriverRef inDriver,
                                 AudioObjectID inDeviceObjectID, UInt32 inClientID,
                                 UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                 const AudioServerPlugInIOCycleInfo *inIOCycleInfo) {
    (void)inDriver;
    (void)inDeviceObjectID;
    (void)inClientID;
    (void)inOperationID;
    (void)inIOBufferFrameSize;
    (void)inIOCycleInfo;
    return noErr;
}

static OSStatus DoIOOperation(AudioServerPlugInDriverRef inDriver,
                              AudioObjectID inDeviceObjectID,
                              AudioObjectID inStreamObjectID, UInt32 inClientID,
                              UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                              const AudioServerPlugInIOCycleInfo *inIOCycleInfo,
                              void *ioMainBuffer, void *ioSecondaryBuffer) {
    Driver *d = (Driver *)inDriver;
    (void)inDeviceObjectID;
    (void)inStreamObjectID;
    (void)inClientID;
    (void)inIOCycleInfo;
    (void)ioSecondaryBuffer;
    if (inOperationID != kAudioServerPlugInIOOperationWriteMix || !ioMainBuffer)
        return noErr;
    if (isBitstream(d))
        bitstreamPush(d, (const int16_t *)ioMainBuffer, inIOBufferFrameSize);
    else
        ringPush(d, (const Float32 *)ioMainBuffer, inIOBufferFrameSize);
    return noErr;
}

static OSStatus EndIOOperation(AudioServerPlugInDriverRef inDriver,
                               AudioObjectID inDeviceObjectID, UInt32 inClientID,
                               UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                               const AudioServerPlugInIOCycleInfo *inIOCycleInfo) {
    (void)inDriver;
    (void)inDeviceObjectID;
    (void)inClientID;
    (void)inOperationID;
    (void)inIOBufferFrameSize;
    (void)inIOCycleInfo;
    return noErr;
}

/* ---------------- CFPlugIn factory ---------------- */

static HRESULT QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface) {
    if (!outInterface) return EINVAL;
    *outInterface = NULL;
    CFUUIDRef uuid = CFUUIDCreateFromUUIDBytes(kCFAllocatorDefault, inUUID);
    Boolean match = CFEqual(uuid, kAudioServerPlugInDriverInterfaceUUID);
    CFRelease(uuid);
    if (!match) return E_NOINTERFACE;
    AddRef(inDriver);
    *outInterface = inDriver;
    return S_OK;
}

static ULONG AddRef(void *inDriver) {
    Driver *d = (Driver *)inDriver;
    return (ULONG)__atomic_add_fetch(&d->refCount, 1, __ATOMIC_RELAXED);
}

static ULONG Release(void *inDriver) {
    Driver *d = (Driver *)inDriver;
    UInt32 n = (UInt32)__atomic_sub_fetch(&d->refCount, 1, __ATOMIC_ACQ_REL);
    if (n == 0) {
        stopWorker(d);
        free(d->ring);
        free(d->bitstreamRing);
        pthread_mutex_destroy(&d->lifecycleLock);
        free(d);
        return 0;
    }
    return n;
}

__attribute__((visibility("default")))
void *Ranhe_CreatePlugin(CFAllocatorRef allocator, CFUUIDRef typeID) {
    (void)allocator;
    halLog("Ranhe_CreatePlugin enter");
    if (!typeID || !CFEqual(typeID, kAudioServerPlugInTypeUUID)) return NULL;
    Driver *d = calloc(1, sizeof(Driver));
    if (!d) return NULL;
    d->vtable = &gVTable;
    d->refCount = 1;
    d->bufferFrames = RH899X_BUFFER_FRAMES;
    atomic_init(&d->rate, kDefaultRate);
    atomic_init(&d->running, false);
    atomic_init(&d->threadStarted, false);
    atomic_init(&d->muted, false);
    atomic_init(&d->volume, 1.0f);
    atomic_init(&d->head, 0);
    atomic_init(&d->tail, 0);
    atomic_init(&d->usbTransfers, 0);
    atomic_init(&d->usbFrames, 0);
    atomic_init(&d->usbErrors, 0);
    atomic_init(&d->frameErrors, 0);
    atomic_init(&d->underruns, 0);
    atomic_init(&d->overruns, 0);
    atomic_init(&d->drops, 0);
    atomic_init(&d->dups, 0);
    atomic_init(&d->startHost, 0);
    atomic_init(&d->seed, 1);
    atomic_init(&d->formatMode, kFormatFloat32);
    atomic_init(&d->supportsMixing, 1);
    atomic_init(&d->bitstreamHead, 0);
    atomic_init(&d->bitstreamTail, 0);
    d->ring = calloc(kRingFrames * kChannels, sizeof(Float32));
    d->bitstreamRing = calloc(kRingFrames * kChannels, sizeof(int16_t));
    if (!d->ring || !d->bitstreamRing) {
        free(d->ring);
        free(d->bitstreamRing);
        free(d);
        return NULL;
    }
    pthread_mutex_init(&d->lifecycleLock, NULL);
    halLog("Ranhe_CreatePlugin result=%p", (void *)d);
    return d;
}


static int usbStartStream(Driver *d, UsbContext *ctx, XferSlot *slots) {
    if (!usbFind(ctx)) return 0;
    if ((*ctx->itf)->USBInterfaceOpenSeize(ctx->itf)) { usbClose(ctx); return 0; }
    mach_port_t port = MACH_PORT_NULL;
    (*ctx->itf)->CreateInterfaceAsyncPort(ctx->itf, &port);
    CFRunLoopSourceRef src = NULL;
    (*ctx->itf)->CreateInterfaceAsyncEventSource(ctx->itf, &src);
    if (src) CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
    UInt8 alt = isBitstream(d) ? 2 : 1;
    if ((*ctx->itf)->SetAlternateInterface(ctx->itf, alt)) { usbClose(ctx); return 0; }
    halLog("USB alt=%u", alt);
    UInt8 endpoints = 0;
    (*ctx->itf)->GetNumEndpoints(ctx->itf, &endpoints);
    halLog("USB endpoints=%u", endpoints);
    for (UInt8 p = 1; p <= endpoints; p++) {
        UInt8 dir = 0, num = 0, type = 0, interval = 0;
        UInt16 maxpkt = 0;
        IOReturn pr = (*ctx->itf)->GetPipeProperties(ctx->itf, p, &dir, &num, &type,
                                                     &maxpkt, &interval);
        halLog("pipe %u rc=0x%x dir=%u ep=%u type=%u maxpkt=%u int=%u",
               p, pr, dir, num, type, maxpkt, interval);
    }

    UInt32 rateHz = atomic_load(&d->rate);
    unsigned char rate[4] = {
        rateHz & 0xff, (rateHz >> 8) & 0xff,
        (rateHz >> 16) & 0xff, (rateHz >> 24) & 0xff
    };
    IOUSBDevRequestTO rq = {0};
    rq.bmRequestType = 0x21;
    rq.bRequest = 0x01;
    rq.wLength = 4;
    rq.pData = rate;
    rq.noDataTimeout = 1000;
    rq.completionTimeout = 1000;
    /* UAC2 form: wValue = CS_SAM_FREQ | ClockID, wIndex = AudioControl IF. */
    rq.wValue = 0x0101;
    rq.wIndex = 0;
    IOReturn rUac2 = (*ctx->dev)->DeviceRequestTO(ctx->dev, &rq);
    /* Some C-Media-style firmwares use the UAC1 unit-ID form. Send both;
     * the request is idempotent and the device ignores the unused form. */
    rq.wValue = 0x0100;
    rq.wIndex = 1;
    IOReturn rLegacy = (*ctx->dev)->DeviceRequestTO(ctx->dev, &rq);
    halLog("USB clock %u Hz uac2=0x%x legacy=0x%x", rateHz, rUac2, rLegacy);

    UInt32 bytes = transferBytesForRate(rateHz);
    for (int i = 0; i < kSlots; i++) {
        slots[i].driver = d;
        slots[i].buf = calloc(1, bytes);
        slots[i].frames = calloc(kMicroframes, sizeof(IOUSBIsocFrame));
        slots[i].bufferBytes = bytes;
        atomic_store(&slots[i].busy, 0);
        if (!slots[i].buf || !slots[i].frames) return 0;
    }
    return 1;
}

static void usbFreeSlots(XferSlot *slots) {
    for (int i = 0; i < kSlots; i++) {
        free(slots[i].buf);
        free(slots[i].frames);
        slots[i].buf = NULL;
        slots[i].frames = NULL;
    }
}

static void usbDrainSlots(XferSlot *slots) {
    /* Transfers already queued in the controller still write frActCount and
     * frStatus into the frame lists. Wait for them before freeing memory. */
    for (int i = 0; i < 100; i++) {
        int busy = 0;
        for (int s = 0; s < kSlots; s++)
            if (atomic_load(&slots[s].busy)) busy = 1;
        if (!busy) break;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, false);
    }
}

static void usbRunStream(Driver *d, UsbContext *ctx, XferSlot *slots) {
    UInt32 streamRate = isBitstream(d) ? kBitstreamRate : atomic_load(&d->rate);
    UInt32 samplesPerMicroframe = samplesPerMicroframeForRate(streamRate);
    UInt32 transferFrames = kMicroframes * samplesPerMicroframe;
    UInt32 microframeBytes = microframeBytesForRate(streamRate);
    UInt64 levelTarget = kLevelFrames(transferFrames, 8) / 3;
    UInt64 levelHigh = kLevelFrames(transferFrames, 4);
    UInt64 levelLow = kLevelFrames(transferFrames, 2);
    Float32 pcm[transferFrames * kChannels];
    int16_t words[transferFrames * kChannels];
    UInt64 nextStart = 0;
    UInt64 lastEnd = 0;
    int consecutiveErrors = 0;

    UInt64 prefillStart = mach_absolute_time();
    UInt64 prefillDeadline = prefillStart + nanosToHostTicks(80000000ULL);
    while (atomic_load(&d->running) &&
           (isBitstream(d) ? bitstreamLevel(d) : ringLevel(d)) < levelHigh &&
           mach_absolute_time() < prefillDeadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, false);
    }
    halLog("prefill bitstream=%d frames=%llu wait_ms=%.1f", isBitstream(d) ? 1 : 0,
           (unsigned long long)(isBitstream(d) ? bitstreamLevel(d) : ringLevel(d)),
           (double)hostTicksToNanos(mach_absolute_time() - prefillStart) / 1e6);

    while (atomic_load(&d->running)) {
        UInt64 cur = 0;
        AbsoluteTime at = {0, 0};
        (*ctx->itf)->GetBusFrameNumber(ctx->itf, &cur, &at);
        if (nextStart == 0 || nextStart < cur + 2) nextStart = cur + 4;
        if (isBitstream(d))
            nextStart = (nextStart + kBitstreamMicroframes - 1) &
                        ~(UInt64)(kBitstreamMicroframes - 1);
        if (nextStart < lastEnd) nextStart = lastEnd;

        while (nextStart <= cur + kMaxAhead) {
            int s = -1;
            for (int i = 0; i < kSlots; i++) {
                if (!atomic_load(&slots[i].busy)) { s = i; break; }
            }
            if (s < 0) break;

            unsigned char *out = slots[s].buf;
            IOUSBIsocFrame *fl = slots[s].frames;
            UInt32 numFrames = kMicroframes;
            if (isBitstream(d)) {
                numFrames = kBitstreamMicroframes;
                UInt32 got = bitstreamPop(d, words, transferFrames);
                if (got < transferFrames)
                    memset(out + got * 4, 0, (transferFrames - got) * 4);
                if (got) memcpy(out, words, got * 4);
                /* Same even shape as PCM: 24 bytes every 125 us. A single
                 * 1536-byte burst per 8 ms leaves 7 ms silent and makes the
                 * downstream decoder click ("telegraph" noise). */
                for (UInt32 m = 0; m < numFrames; m++) {
                    fl[m].frReqCount = 192;
                    fl[m].frStatus = 0;
                    fl[m].frActCount = 0;
                }
            } else {
                UInt32 got = ringPop(d, pcm, transferFrames);
                float gain = atomic_load(&d->volume);
                if (atomic_load(&d->muted)) gain = 0;
                for (UInt32 f = 0; f < transferFrames; f++) {
                    float l = f < got ? pcm[f * kChannels] : 0;
                    float r = f < got ? pcm[f * kChannels + 1] : 0;
                    if (l > 1) l = 1; else if (l < -1) l = -1;
                    if (r > 1) r = 1; else if (r < -1) r = -1;
                    short sl = (short)lrintf(l * gain * 32767.0f);
                    short sr = (short)lrintf(r * gain * 32767.0f);
                    unsigned char *o = out + f * 4;
                    o[0] = sl & 0xff; o[1] = (sl >> 8) & 0xff;
                    o[2] = sr & 0xff; o[3] = (sr >> 8) & 0xff;
                }
                for (UInt32 m = 0; m < kMicroframes; m++) {
                    fl[m].frReqCount = microframeBytes;
                    fl[m].frStatus = 0;
                    fl[m].frActCount = 0;
                }
            }
            slots[s].startFrame = nextStart;
            slots[s].numFrames = numFrames;
            atomic_store(&slots[s].busy, 1);
            IOReturn r = (*ctx->itf)->WriteIsochPipeAsync(ctx->itf, 1, out, nextStart,
                                                          numFrames, fl, usbDone,
                                                          &slots[s]);
            if (r) {
                atomic_store(&slots[s].busy, 0);
                atomic_fetch_add(&d->usbErrors, 1);
                halLog("usb submit failed 0x%x start=%llu mode=%d",
                       r, (unsigned long long)nextStart, atomic_load(&d->formatMode));
                if (++consecutiveErrors >= 3) return;
                nextStart = 0;              /* resync to the current bus frame */
                lastEnd = 0;
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, false);
                break;
            }
            consecutiveErrors = 0;
            lastEnd = nextStart + kMicroframes / 8;
            nextStart += kMicroframes / 8;
        }

        UInt64 level = isBitstream(d) ? bitstreamLevel(d) : ringLevel(d);
        if (level > levelHigh) {
            UInt64 excess = level - levelTarget;
            atomic_fetch_add(isBitstream(d) ? &d->bitstreamTail : &d->tail, excess);
            atomic_fetch_add(&d->drops, excess);
        } else if (level < levelLow) {
            UInt64 tail = atomic_load(isBitstream(d) ? &d->bitstreamTail : &d->tail);
            UInt64 head = atomic_load(isBitstream(d) ? &d->bitstreamHead : &d->head);
            if (tail < head)
                atomic_fetch_sub(isBitstream(d) ? &d->bitstreamTail : &d->tail, 1);
            atomic_fetch_add(&d->dups, 1);
        }
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0005, false);
    }
}

static void *usbThread(void *arg) {
    Driver *d = (Driver *)arg;
    while (atomic_load(&d->running)) {
        UsbContext ctx;
        XferSlot slots[kSlots];
        memset(&ctx, 0, sizeof(ctx));
        memset(slots, 0, sizeof(slots));
        if (!usbStartStream(d, &ctx, slots)) {
            usbFreeSlots(slots);
            usleep(100000);
            if (!atomic_load(&d->running)) break;
            continue;
        }
        usbRunStream(d, &ctx, slots);
        usbDrainSlots(slots);
        usbFreeSlots(slots);
        usbClose(&ctx);
        if (atomic_load(&d->running)) usleep(100000);
    }
    return NULL;
}

/* ---------------- AudioServerPlugIn object model ---------------- */

static Boolean isOutputScope(AudioObjectPropertyScope scope) {
    return scope == kAudioObjectPropertyScopeGlobal ||
           scope == kAudioObjectPropertyScopeOutput;
}

static Boolean hasProperty(AudioObjectID obj, AudioObjectPropertySelector sel,
                           AudioObjectPropertyScope scope) {
    if (scope == kAudioObjectPropertyScopeInput) {
        return obj == kDeviceID &&
               (sel == kAudioDevicePropertyStreams ||
                sel == kAudioDevicePropertyStreamConfiguration ||
                sel == kAudioDevicePropertyLatency ||
                sel == kAudioDevicePropertySafetyOffset);
    }
    if (!isOutputScope(scope)) return false;
    switch (obj) {
        case kAudioObjectPlugInObject:
            switch (sel) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyName:
                case kAudioObjectPropertyManufacturer:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioPlugInPropertyDeviceList:
                case kAudioPlugInPropertyTranslateUIDToDevice:
                case kAudioPlugInPropertyResourceBundle:
                case kAudioPlugInPropertyBoxList:
                case kAudioPlugInPropertyClockDeviceList:
                case kAudioObjectPropertyCustomPropertyInfoList:
                    return true;
                default:
                    return false;
            }
        case kDeviceID:
            switch (sel) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyName:
                case kAudioObjectPropertyManufacturer:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioDevicePropertyDeviceUID:
                case kAudioDevicePropertyModelUID:
                case kAudioDevicePropertyTransportType:
                case kAudioDevicePropertyClockDomain:
                case kAudioDevicePropertyDeviceIsAlive:
                case kAudioDevicePropertyDeviceIsRunning:
                case kAudioDevicePropertyDeviceIsRunningSomewhere:
                case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                case kAudioDevicePropertyLatency:
                case kAudioDevicePropertySafetyOffset:
                case kAudioDevicePropertyNominalSampleRate:
                case kAudioDevicePropertyAvailableNominalSampleRates:
                case kAudioDevicePropertyStreams:
                case kAudioDevicePropertyStreamConfiguration:
                case kAudioDevicePropertyStreamFormat:
                case kAudioDevicePropertyStreamFormats:
                case kAudioObjectPropertyControlList:
                case kAudioDevicePropertyIsHidden:
                case kAudioDevicePropertyRelatedDevices:
                case kAudioDevicePropertyPreferredChannelLayout:
                case kAudioDevicePropertyPreferredChannelsForStereo:
                case kAudioDevicePropertyZeroTimeStampPeriod:
                case kAudioDevicePropertyVolumeScalar:
                case kAudioDevicePropertyMute:
                case kAudioDevicePropertySupportsMixing:
                case kAudioDevicePropertyBufferFrameSize:
                case kAudioDevicePropertyBufferFrameSizeRange:
                case kRanheStatsSelector:
                    return true;
                default:
                    return false;
            }
        case kStreamID:
            switch (sel) {
                case kAudioObjectPropertyBaseClass:
                case kAudioObjectPropertyClass:
                case kAudioObjectPropertyOwner:
                case kAudioObjectPropertyName:
                case kAudioObjectPropertyOwnedObjects:
                case kAudioStreamPropertyIsActive:
                case kAudioStreamPropertyDirection:
                case kAudioStreamPropertyTerminalType:
                case kAudioStreamPropertyStartingChannel:
                case kAudioStreamPropertyLatency:
                case kAudioStreamPropertyVirtualFormat:
                case kAudioStreamPropertyPhysicalFormat:
                case kAudioStreamPropertyAvailableVirtualFormats:
                case kAudioStreamPropertyAvailablePhysicalFormats:
                    return true;
                default:
                    return false;
            }
        default:
            return false;
    }
}

static Boolean HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                           pid_t inClientProcessID,
                           const AudioObjectPropertyAddress *inAddress) {
    (void)inDriver;
    (void)inClientProcessID;
    if (!inAddress) return false;
    return hasProperty(inObjectID, inAddress->mSelector, inAddress->mScope);
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef inDriver,
                                   AudioObjectID inObjectID, pid_t inClientProcessID,
                                   const AudioObjectPropertyAddress *inAddress,
                                   Boolean *outIsSettable) {
    Driver *d = (Driver *)inDriver;
    (void)inClientProcessID;
    if (!inAddress || !outIsSettable) return kAudio_ParamError;
    *outIsSettable = false;
    if (inObjectID == kDeviceID) {
        if (inAddress->mSelector == kAudioDevicePropertyVolumeScalar ||
            inAddress->mSelector == kAudioDevicePropertyMute ||
            inAddress->mSelector == kAudioDevicePropertySupportsMixing ||
            inAddress->mSelector == kAudioDevicePropertyNominalSampleRate ||
            inAddress->mSelector == kAudioDevicePropertyBufferFrameSize)
            *outIsSettable = true;
    } else if (inObjectID == kStreamID) {
        if (inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
            inAddress->mSelector == kAudioStreamPropertyPhysicalFormat)
            *outIsSettable = !atomic_load(&d->running);
    }
    return noErr;
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef inDriver,
                                    AudioObjectID inObjectID, pid_t inClientProcessID,
                                    const AudioObjectPropertyAddress *inAddress,
                                    UInt32 inQualifierDataSize,
                                    const void *inQualifierData, UInt32 *outDataSize) {
    (void)inDriver;
    (void)inClientProcessID;
    (void)inQualifierDataSize;
    (void)inQualifierData;
    if (!inAddress || !outDataSize) return kAudio_ParamError;
    halLog("size obj=%u sel=%c%c%c%c scope=%c%c%c%c", inObjectID,
           (char)(inAddress->mSelector >> 24), (char)(inAddress->mSelector >> 16),
           (char)(inAddress->mSelector >> 8), (char)inAddress->mSelector,
           (char)(inAddress->mScope >> 24), (char)(inAddress->mScope >> 16),
           (char)(inAddress->mScope >> 8), (char)inAddress->mScope);
    if (!hasProperty(inObjectID, inAddress->mSelector, inAddress->mScope))
        return kAudioHardwareUnknownPropertyError;

    switch (inAddress->mSelector) {
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
            *outDataSize = sizeof(CFStringRef);
            return noErr;
        case kAudioPlugInPropertyResourceBundle:
            *outDataSize = sizeof(CFStringRef);
            return noErr;
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
            *outDataSize = sizeof(AudioObjectID);
            return noErr;
        case kAudioObjectPropertyOwnedObjects:
            if (inObjectID == kStreamID) *outDataSize = 0;
            else *outDataSize = sizeof(AudioObjectID);
            return noErr;
        case kAudioDevicePropertyStreams:
            *outDataSize = isOutputScope(inAddress->mScope) ? sizeof(AudioObjectID) : 0;
            return noErr;
        case kAudioDevicePropertyStreamConfiguration:
            *outDataSize = sizeof(AudioBufferList);
            return noErr;
        case kAudioDevicePropertyAvailableNominalSampleRates:
            *outDataSize = kAdvertisedRateCount * sizeof(AudioValueRange);
            return noErr;
        case kAudioDevicePropertyBufferFrameSizeRange:
            *outDataSize = sizeof(AudioValueRange);
            return noErr;
        case kAudioDevicePropertyPreferredChannelLayout:
            *outDataSize = offsetof(AudioChannelLayout, mChannelDescriptions);
            return noErr;
        case kAudioDevicePropertyPreferredChannelsForStereo:
            *outDataSize = 2 * sizeof(UInt32);
            return noErr;
        case kAudioDevicePropertyNominalSampleRate:
            *outDataSize = sizeof(Float64);
            return noErr;
        case kAudioDevicePropertyVolumeScalar:
            *outDataSize = sizeof(Float32);
            return noErr;
        case kAudioDevicePropertyStreamFormat:
        case kAudioDevicePropertyStreamFormats:
        case kAudioStreamPropertyPhysicalFormat:
            *outDataSize = sizeof(AudioStreamBasicDescription);
            return noErr;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *outDataSize = (2 * kAdvertisedRateCount + 1) *
                           sizeof(AudioStreamRangedDescription);
            return noErr;
        case kRanheStatsSelector:
            *outDataSize = sizeof(RanheStats);
            return noErr;
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceIsRunningSomewhere:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyMute:
        case kAudioDevicePropertySupportsMixing:
        case kAudioDevicePropertyBufferFrameSize:
        case kAudioDevicePropertyIsHidden:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
            *outDataSize = sizeof(UInt32);
            return noErr;
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioPlugInPropertyBoxList:
        case kAudioPlugInPropertyClockDeviceList:
        case kAudioObjectPropertyCustomPropertyInfoList:
            *outDataSize = 0;
            return noErr;
        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus GetPropertyData(AudioServerPlugInDriverRef inDriver,
                                AudioObjectID inObjectID, pid_t inClientProcessID,
                                const AudioObjectPropertyAddress *inAddress,
                                UInt32 inQualifierDataSize,
                                const void *inQualifierData,
                                UInt32 inDataSize, UInt32 *outDataSize,
                                void *outData) {
    Driver *d = (Driver *)inDriver;
    (void)inClientProcessID;
    if (!inAddress || !outDataSize || (!outData && inDataSize)) return kAudio_ParamError;
    halLog("data obj=%u sel=%c%c%c%c scope=%c%c%c%c in=%u", inObjectID,
           (char)(inAddress->mSelector >> 24), (char)(inAddress->mSelector >> 16),
           (char)(inAddress->mSelector >> 8), (char)inAddress->mSelector,
           (char)(inAddress->mScope >> 24), (char)(inAddress->mScope >> 16),
           (char)(inAddress->mScope >> 8), (char)inAddress->mScope, inDataSize);
    if (!hasProperty(inObjectID, inAddress->mSelector, inAddress->mScope))
        return kAudioHardwareUnknownPropertyError;

    UInt32 need = 0;
    OSStatus err = GetPropertyDataSize(inDriver, inObjectID, inClientProcessID,
                                       inAddress, inQualifierDataSize, inQualifierData,
                                       &need);
    if (err) return err;
    if (inDataSize < need) return kAudioHardwareBadPropertySizeError;
    *outDataSize = need;
    halLog("data need=%u in=%u", need, inDataSize);
    if (inDataSize == 0) return noErr;

    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
            *(AudioClassID *)outData = kAudioObjectClassID;
            break;
        case kAudioObjectPropertyClass:
            *(AudioClassID *)outData = inObjectID == kDeviceID ? kAudioDeviceClassID :
                (inObjectID == kStreamID ? kAudioStreamClassID : kAudioPlugInClassID);
            break;
        case kAudioObjectPropertyOwner:
            *(AudioObjectID *)outData = inObjectID == kDeviceID ? kAudioObjectPlugInObject :
                (inObjectID == kStreamID ? kDeviceID : kAudioObjectSystemObject);
            break;
        case kAudioObjectPropertyName:
            *(CFStringRef *)outData = inObjectID == kDeviceID ? kDeviceName :
                (inObjectID == kStreamID ? CFSTR("RH-899X Output") : kDeviceName);
            break;
        case kAudioObjectPropertyManufacturer:
            *(CFStringRef *)outData = kManufacturer;
            break;
        case kAudioDevicePropertyDeviceUID:
            *(CFStringRef *)outData = kUID;
            break;
        case kAudioDevicePropertyModelUID:
            *(CFStringRef *)outData = kModelUID;
            break;
        case kAudioPlugInPropertyDeviceList:
            *(AudioObjectID *)outData = kDeviceID;
            halLog("DeviceList -> 0x%x", kDeviceID);
            break;
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            CFStringRef uid = NULL;
            if (inQualifierData && inQualifierDataSize >= sizeof(CFStringRef))
                uid = *(CFStringRef *)inQualifierData;
            *(AudioObjectID *)outData =
                (uid && CFGetTypeID(uid) == CFStringGetTypeID() && CFEqual(uid, kUID))
                    ? kDeviceID : kAudioObjectUnknown;
            break;
        }
        case kAudioPlugInPropertyResourceBundle:
            /* HAL contract: a CFString path relative to the plug-in bundle. */
            *(CFStringRef *)outData = CFSTR("Contents/Resources");
            break;
        case kAudioObjectPropertyOwnedObjects:
            if (inObjectID == kStreamID) {
                *outDataSize = 0;
            } else {
                *(AudioObjectID *)outData = inObjectID == kDeviceID ? kStreamID : kDeviceID;
                *outDataSize = sizeof(AudioObjectID);
            }
            break;
        case kAudioDevicePropertyStreams:
            if (isOutputScope(inAddress->mScope)) *(AudioObjectID *)outData = kStreamID;
            else *outDataSize = 0;
            break;
        case kAudioDevicePropertyStreamConfiguration: {
            AudioBufferList *list = (AudioBufferList *)outData;
            if (!isOutputScope(inAddress->mScope)) {
                list->mNumberBuffers = 0;
                break;
            }
            list->mNumberBuffers = 1;
            list->mBuffers[0].mNumberChannels = kChannels;
            list->mBuffers[0].mDataByteSize = 0;
            list->mBuffers[0].mData = NULL;
            break;
        }
        case kAudioDevicePropertyNominalSampleRate:
            *(Float64 *)outData = atomic_load(&d->rate);
            break;
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            AudioValueRange *r = (AudioValueRange *)outData;
            for (UInt32 i = 0; i < kAdvertisedRateCount; i++) {
                r[i].mMinimum = kSupportedRates[i];
                r[i].mMaximum = kSupportedRates[i];
            }
            break;
        }
        case kAudioDevicePropertyBufferFrameSizeRange: {
            AudioValueRange *r = (AudioValueRange *)outData;
            r->mMinimum = 128;
            r->mMaximum = 4096;
            break;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            AudioChannelLayout *l = (AudioChannelLayout *)outData;
            memset(l, 0, need);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
            break;
        }
        case kAudioDevicePropertyPreferredChannelsForStereo:
            ((UInt32 *)outData)[0] = 1;
            ((UInt32 *)outData)[1] = 2;
            break;
        case kAudioDevicePropertyTransportType:
            *(UInt32 *)outData = kAudioDeviceTransportTypeUSB;
            break;
        case kAudioDevicePropertyClockDomain:
            *(UInt32 *)outData = 0;
            break;
        case kAudioDevicePropertyDeviceIsAlive:
            *(UInt32 *)outData = 1;
            break;
        case kAudioDevicePropertyDeviceIsRunning:
            *(UInt32 *)outData = atomic_load(&d->running) ? 1 : 0;
            break;
        case kAudioDevicePropertyDeviceIsRunningSomewhere:
            *(UInt32 *)outData = d->ioClients ? 1 : 0;
            break;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            *(UInt32 *)outData = 1;
            break;
        case kAudioDevicePropertyLatency:
            *(UInt32 *)outData = isBitstream(d) ? RH899X_LATENCY_FRAMES : 0;
            break;
        case kAudioDevicePropertySafetyOffset:
            *(UInt32 *)outData = isBitstream(d) ? RH899X_SAFETY_FRAMES : 0;
            break;
        case kAudioDevicePropertyZeroTimeStampPeriod:
            *(UInt32 *)outData = d->bufferFrames;
            break;
        case kAudioDevicePropertyBufferFrameSize:
            *(UInt32 *)outData = d->bufferFrames;
            break;
        case kAudioDevicePropertyIsHidden:
            *(UInt32 *)outData = 0;
            break;
        case kAudioDevicePropertyVolumeScalar:
            *(Float32 *)outData = atomic_load(&d->volume);
            break;
        case kAudioDevicePropertyMute:
            *(UInt32 *)outData = atomic_load(&d->muted) ? 1 : 0;
            break;
        case kAudioDevicePropertySupportsMixing:
            *(UInt32 *)outData = atomic_load(&d->supportsMixing) ? 1 : 0;
            break;
        case kAudioDevicePropertyStreamFormat:
        case kAudioDevicePropertyStreamFormats:
        case kAudioStreamPropertyPhysicalFormat:
            setASBD(d, (AudioStreamBasicDescription *)outData);
            break;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription *r = (AudioStreamRangedDescription *)outData;
            UInt32 n = 0;
            for (int mode = 0; mode < 2; mode++) {
                for (UInt32 i = 0; i < kAdvertisedRateCount; i++) {
                    setASBDForMode(&r[n].mFormat, mode, kSupportedRates[i]);
                    r[n].mSampleRateRange.mMinimum = kSupportedRates[i];
                    r[n].mSampleRateRange.mMaximum = kSupportedRates[i];
                    n++;
                }
            }
            setASBDForMode(&r[n].mFormat, kFormat60958AC3, kBitstreamRate);
            r[n].mSampleRateRange.mMinimum = kBitstreamRate;
            r[n].mSampleRateRange.mMaximum = kBitstreamRate;
            n++;
            break;
        }
        case kAudioStreamPropertyIsActive:
            *(UInt32 *)outData = 1;
            break;
        case kAudioStreamPropertyDirection:
            *(UInt32 *)outData = 0; /* output */
            break;
        case kAudioStreamPropertyTerminalType:
            *(UInt32 *)outData = isBitstream(d)
                ? kAudioStreamTerminalTypeDigitalAudioInterface
                : kAudioStreamTerminalTypeSpeaker;
            break;
        case kAudioStreamPropertyStartingChannel:
            *(UInt32 *)outData = 1;
            break;
            *(UInt32 *)outData = 0;
            break;
        case kRanheStatsSelector: {
            RanheStats *s = (RanheStats *)outData;
            memset(s, 0, sizeof(*s));
            s->usbTransfers = atomic_load(&d->usbTransfers);
            s->usbFrames = atomic_load(&d->usbFrames);
            s->usbErrors = atomic_load(&d->usbErrors);
            s->frameErrors = atomic_load(&d->frameErrors);
            s->underruns = atomic_load(&d->underruns);
            s->overruns = atomic_load(&d->overruns);
            s->drops = atomic_load(&d->drops);
            s->dups = atomic_load(&d->dups);
            s->ringLevel = (UInt32)(isBitstream(d) ? bitstreamLevel(d) : ringLevel(d));
            s->ioClients = d->ioClients;
            break;
        }
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioPlugInPropertyBoxList:
        case kAudioPlugInPropertyClockDeviceList:
        case kAudioObjectPropertyCustomPropertyInfoList:
            *outDataSize = 0;
            break;
        default:
            return kAudioHardwareUnknownPropertyError;
    }
    return noErr;
}

static void notifyProperty(Driver *d, AudioObjectID obj,
                           AudioObjectPropertySelector sel,
                           AudioObjectPropertyScope scope,
                           AudioObjectPropertyElement element) {
    if (!d->host || !d->host->PropertiesChanged) return;
    AudioObjectPropertyAddress addr = {sel, scope, element};
    d->host->PropertiesChanged(d->host, obj, 1, &addr);
}

static OSStatus applyStreamFormat(Driver *d, const AudioStreamBasicDescription *asbd) {
    if (atomic_load(&d->running))
        return kAudioHardwareUnsupportedOperationError;
    if (asbd->mChannelsPerFrame != kChannels)
        return kAudioDeviceUnsupportedFormatError;
    int mode;
    if (asbd->mFormatID == kAudioFormatLinearPCM &&
        (asbd->mFormatFlags & kAudioFormatFlagIsFloat)) {
        mode = kFormatFloat32;
    } else if (asbd->mFormatID == kAudioFormatLinearPCM &&
               asbd->mBitsPerChannel == 16 &&
               (asbd->mFormatFlags & kAudioFormatFlagIsNonMixable)) {
        mode = kFormatPcm16NonMixable;
    } else if (asbd->mFormatID == kAudioFormat60958AC3 &&
               asbd->mBitsPerChannel == 16) {
        if (asbd->mSampleRate != kBitstreamRate)
            return kAudioDeviceUnsupportedFormatError;
        if (atomic_load(&d->rate) != kBitstreamRate)
            return kAudioDeviceUnsupportedFormatError;
        mode = kFormat60958AC3;
    } else {
        return kAudioDeviceUnsupportedFormatError;
    }
    if (mode != kFormat60958AC3 && asbd->mSampleRate != atomic_load(&d->rate))
        return kAudioDeviceUnsupportedFormatError;
    atomic_store(&d->formatMode, mode);
    halLog("stream format mode=%d", mode);
    notifyProperty(d, kStreamID, kAudioStreamPropertyVirtualFormat,
                   kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain);
    notifyProperty(d, kStreamID, kAudioStreamPropertyPhysicalFormat,
                   kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain);
    notifyProperty(d, kDeviceID, kAudioDevicePropertyStreamFormat,
                   kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain);
    return noErr;
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef inDriver,
                                AudioObjectID inObjectID, pid_t inClientProcessID,
                                const AudioObjectPropertyAddress *inAddress,
                                UInt32 inQualifierDataSize,
                                const void *inQualifierData, UInt32 inDataSize,
                                const void *inData) {
    Driver *d = (Driver *)inDriver;
    (void)inClientProcessID;
    (void)inQualifierDataSize;
    (void)inQualifierData;
    if (!inAddress || !inData) return kAudio_ParamError;

    if (inObjectID == kStreamID) {
        if (inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
            inAddress->mSelector == kAudioStreamPropertyPhysicalFormat) {
            if (inDataSize < sizeof(AudioStreamBasicDescription))
                return kAudioHardwareBadPropertySizeError;
            return applyStreamFormat(d, (const AudioStreamBasicDescription *)inData);
        }
        return kAudioHardwareUnknownPropertyError;
    }
    if (inObjectID != kDeviceID) return kAudioHardwareUnknownPropertyError;

    switch (inAddress->mSelector) {
        case kAudioDevicePropertyNominalSampleRate: {
            if (inDataSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
            Float64 requested = *(const Float64 *)inData;
            if (!rateIsSupported(requested))
                return kAudioDeviceUnsupportedFormatError;
            if (atomic_load(&d->running))
                return kAudioHardwareUnsupportedOperationError;
            if (atomic_load(&d->formatMode) == kFormat60958AC3 &&
                (UInt32)requested != kBitstreamRate)
                return kAudioDeviceUnsupportedFormatError;
            atomic_store(&d->rate, (UInt32)requested);
            halLog("nominal rate=%u", (UInt32)requested);
            notifyProperty(d, kDeviceID, kAudioDevicePropertyNominalSampleRate,
                           kAudioObjectPropertyScopeGlobal,
                           kAudioObjectPropertyElementMain);
            notifyProperty(d, kStreamID, kAudioStreamPropertyVirtualFormat,
                           kAudioObjectPropertyScopeGlobal,
                           kAudioObjectPropertyElementMain);
            notifyProperty(d, kStreamID, kAudioStreamPropertyPhysicalFormat,
                           kAudioObjectPropertyScopeGlobal,
                           kAudioObjectPropertyElementMain);
            notifyProperty(d, kDeviceID, kAudioDevicePropertyStreamFormat,
                           kAudioObjectPropertyScopeGlobal,
                           kAudioObjectPropertyElementMain);
            return noErr;
        }
        case kAudioDevicePropertyBufferFrameSize: {
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            UInt32 n = *(const UInt32 *)inData;
            if (n < 128 || n > 4096) return kAudioHardwareBadPropertySizeError;
            d->bufferFrames = n;
            return noErr;
        }
        case kAudioDevicePropertySupportsMixing: {
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            atomic_store(&d->supportsMixing, *(const UInt32 *)inData != 0);
            return noErr;
        }
        case kAudioDevicePropertyStreamFormat:
            if (inDataSize < sizeof(AudioStreamBasicDescription))
                return kAudioHardwareBadPropertySizeError;
            return applyStreamFormat(d, (const AudioStreamBasicDescription *)inData);
        case kAudioDevicePropertyVolumeScalar: {
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            float v = *(const Float32 *)inData;
            if (v < 0) v = 0;
            if (v > 1) v = 1;
            atomic_store(&d->volume, v);
            notifyProperty(d, kDeviceID, kAudioDevicePropertyVolumeScalar,
                           kAudioObjectPropertyScopeOutput, inAddress->mElement);
            return noErr;
        }
        case kAudioDevicePropertyMute: {
            if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            atomic_store(&d->muted, *(const UInt32 *)inData != 0);
            notifyProperty(d, kDeviceID, kAudioDevicePropertyMute,
                           kAudioObjectPropertyScopeOutput, inAddress->mElement);
            return noErr;
        }
        default:
            return kAudioHardwareUnknownPropertyError;
    }
}


