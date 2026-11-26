#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define kRingFrames 32768u
#define kRingMask (kRingFrames - 1u)
#define kRate 48000u
#define kChannels 2u
#define kDtsBlockBytes 2048u

typedef struct {
    int16_t data[kRingFrames * kChannels];
    atomic_uint writeIndex;
    atomic_uint readIndex;
} Ring;

typedef struct {
    Ring ring;
    FILE *input;
    atomic_bool running;
    uint64_t underruns;
    atomic_ullong ioCalls;
    atomic_ullong ioFrames;
    atomic_ullong ioCopied;
    atomic_int firstBuffers;
    atomic_uint firstBytes;
    atomic_uint firstChannels;
    UInt32 framesPerCallback;
} TestState;

static void fail(const char *what, OSStatus status) {
    fprintf(stderr, "%s failed: %d (0x%x)\n", what, (int)status, (unsigned)status);
    exit(1);
}

static AudioDeviceID findDevice(CFStringRef wantedUID) {
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size) != noErr)
        return kAudioObjectUnknown;
    AudioDeviceID *devices = calloc(1, size);
    if (!devices) return kAudioObjectUnknown;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, devices);
    AudioDeviceID found = kAudioObjectUnknown;
    for (UInt32 i = 0; i < size / sizeof(AudioDeviceID); i++) {
        AudioObjectPropertyAddress ua = {
            kAudioDevicePropertyDeviceUID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        CFStringRef uid = NULL;
        UInt32 us = sizeof(uid);
        if (AudioObjectGetPropertyData(devices[i], &ua, 0, NULL, &us, &uid) == noErr && uid) {
            if (CFEqual(uid, wantedUID)) found = devices[i];
            CFRelease(uid);
        }
        if (found != kAudioObjectUnknown) break;
    }
    free(devices);
    return found;
}

static AudioStreamID firstOutputStream(AudioDeviceID device) {
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreams,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &addr, 0, NULL, &size) != noErr || !size)
        return kAudioObjectUnknown;
    AudioStreamID stream = kAudioObjectUnknown;
    AudioObjectGetPropertyData(device, &addr, 0, NULL, &size, &stream);
    return stream;
}

static OSStatus readBlock(TestState *s, int16_t *dst) {
    uint8_t frame[kDtsBlockBytes];
    if (fread(frame, 1, sizeof(frame), s->input) != sizeof(frame)) return 1;
    if (memcmp(frame, "\x7f\xfe\x80\x01", 4) != 0) return 2;

    unsigned blocks = (((frame[4] << 8) | frame[5]) >> 2) & 0x7f;
    unsigned coreSize = (((frame[5] << 16) | (frame[6] << 8) | frame[7]) >> 4) & 0x3fff;
    blocks++;
    coreSize++;
    unsigned samples = blocks << 5;
    if (samples != 512 && samples != 1024 && samples != 2048) return 3;
    if (coreSize > kDtsBlockBytes - 8) return 4;
    unsigned type = samples == 512 ? 0x0B : (samples == 1024 ? 0x0C : 0x0D);

    uint8_t packet[kDtsBlockBytes];
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x72; packet[1] = 0xF8;       /* Pa */
    packet[2] = 0x1F; packet[3] = 0x4E;       /* Pb */
    packet[4] = type; packet[5] = 0x00;       /* Pc */
    unsigned lengthBits = coreSize << 3;
    packet[6] = lengthBits & 0xff;
    packet[7] = (lengthBits >> 8) & 0xff;     /* Pd */

    for (unsigned i = 0; i < coreSize; i += 2) {
        packet[8 + i] = frame[i + 1];
        packet[9 + i] = frame[i];
    }
    memcpy(dst, packet, sizeof(packet));
    return 0;
}

static OSStatus bitstreamIOProc(AudioObjectID device, const AudioTimeStamp *now,
                                const AudioBufferList *inputData,
                                const AudioTimeStamp *inputTime,
                                AudioBufferList *outputData,
                                const AudioTimeStamp *outputTime, void *refCon) {
    (void)device; (void)now; (void)inputData; (void)inputTime;
    (void)outputTime;
    TestState *s = (TestState *)refCon;
    if (!outputData || outputData->mNumberBuffers == 0) return noErr;
    AudioBuffer *buf = &outputData->mBuffers[0];
    if (atomic_load(&s->ioCalls) == 0) {
        atomic_store(&s->firstBuffers, (int)outputData->mNumberBuffers);
        atomic_store(&s->firstBytes, (unsigned)buf->mDataByteSize);
        atomic_store(&s->firstChannels, (unsigned)buf->mNumberChannels);
    }
    int16_t *dst = (int16_t *)buf->mData;
    if (!dst) return noErr;
    UInt32 frames = s->framesPerCallback;
    unsigned wr = atomic_load(&s->ring.writeIndex);
    unsigned rd = atomic_load(&s->ring.readIndex);
    unsigned available = wr - rd;
    unsigned copy = available < frames ? available : frames;
    atomic_fetch_add(&s->ioCalls, 1);
    atomic_fetch_add(&s->ioFrames, frames);
    atomic_fetch_add(&s->ioCopied, copy);
    for (unsigned i = 0; i < copy; i++) {
        unsigned idx = (rd + i) & kRingMask;
        dst[i * 2] = s->ring.data[idx * 2];
        dst[i * 2 + 1] = s->ring.data[idx * 2 + 1];
    }
    if (copy < frames) {
        memset(dst + copy * 2, 0, (frames - copy) * 2 * sizeof(int16_t));
        s->underruns += frames - copy;
    }
    atomic_store(&s->ring.readIndex, rd + copy);
    return noErr;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s file.dts [seconds]\n", argv[0]);
        return 2;
    }
    double seconds = argc > 2 ? atof(argv[2]) : 5.0;
    TestState state;
    memset(&state, 0, sizeof(state));
    atomic_init(&state.ring.writeIndex, 0);
    atomic_init(&state.ring.readIndex, 0);
    atomic_init(&state.running, true);
    atomic_init(&state.ioCalls, 0);
    atomic_init(&state.ioFrames, 0);
    atomic_init(&state.ioCopied, 0);
    atomic_init(&state.firstBuffers, 0);
    atomic_init(&state.firstBytes, 0);
    atomic_init(&state.firstChannels, 0);
    state.framesPerCallback = 0;
    state.input = fopen(argv[1], "rb");
    if (!state.input) { perror(argv[1]); return 2; }

    CFStringRef uid = CFSTR("BarrotBT10-0c1c-3a1d");
    AudioDeviceID device = findDevice(uid);
    if (device == kAudioObjectUnknown) {
        fprintf(stderr, "Barrot BT10 not found\n");
        return 2;
    }
    AudioStreamID stream = firstOutputStream(device);
    if (stream == kAudioObjectUnknown) {
        fprintf(stderr, "no output stream\n");
        return 2;
    }

    AudioStreamBasicDescription fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.mSampleRate = kRate;
    fmt.mFormatID = kAudioFormat60958AC3;
    fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger |
                       kAudioFormatFlagIsPacked |
                       kAudioFormatFlagIsNonMixable;
    fmt.mBytesPerPacket = 6144;
    fmt.mFramesPerPacket = 1536;
    fmt.mBytesPerFrame = 0;
    fmt.mChannelsPerFrame = kChannels;
    fmt.mBitsPerChannel = 16;

    AudioObjectPropertyAddress ma = {
        kAudioDevicePropertySupportsMixing,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 oldMix = 1;
    UInt32 mixSize = sizeof(oldMix);
    AudioObjectGetPropertyData(device, &ma, 0, NULL, &mixSize, &oldMix);
    AudioObjectPropertyAddress ha = {
        kAudioDevicePropertyHogMode,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    pid_t oldHog = -1;
    UInt32 hogSize = sizeof(oldHog);
    AudioObjectGetPropertyData(device, &ha, 0, NULL, &hogSize, &oldHog);
    pid_t me = getpid();
    OSStatus hs = AudioObjectSetPropertyData(device, &ha, 0, NULL, sizeof(me), &me);
    printf("set hog mode: %d\n", (int)hs);
    UInt32 off = 0;
    AudioObjectSetPropertyData(device, &ma, 0, NULL, sizeof(off), &off);

    AudioObjectPropertyAddress pa = {
        kAudioStreamPropertyPhysicalFormat,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectPropertyAddress va = {
        kAudioStreamPropertyVirtualFormat,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioStreamBasicDescription oldFmt;
    AudioStreamBasicDescription actual;
    UInt32 actualSize = sizeof(actual);
    UInt32 oldSize = sizeof(oldFmt);
    AudioObjectGetPropertyData(stream, &pa, 0, NULL, &oldSize, &oldFmt);
    OSStatus status = AudioObjectSetPropertyData(stream, &pa, 0, NULL, sizeof(fmt), &fmt);
    if (status != noErr) fail("set non-mixable 16-bit format", status);
    printf("set physical cac3 format: %d\n", (int)status);
    if (AudioObjectGetPropertyData(stream, &pa, 0, NULL, &actualSize, &actual) == noErr)
        printf("physical format: id=%c%c%c%c bits=%u bytes/frame=%u flags=0x%x\n",
               (char)(actual.mFormatID >> 24), (char)(actual.mFormatID >> 16),
               (char)(actual.mFormatID >> 8), (char)actual.mFormatID,
               actual.mBitsPerChannel, actual.mBytesPerFrame, actual.mFormatFlags);
    actualSize = sizeof(actual);
    if (AudioObjectGetPropertyData(stream, &va, 0, NULL, &actualSize, &actual) == noErr)
        printf("virtual format: id=%c%c%c%c bits=%u bytes/frame=%u flags=0x%x\n",
               (char)(actual.mFormatID >> 24), (char)(actual.mFormatID >> 16),
               (char)(actual.mFormatID >> 8), (char)actual.mFormatID,
               actual.mBitsPerChannel, actual.mBytesPerFrame, actual.mFormatFlags);

    UInt32 bufferFrames = 1536;
    AudioObjectPropertyAddress ba = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectSetPropertyData(device, &ba, 0, NULL, sizeof(bufferFrames), &bufferFrames);
    UInt32 actualBufferFrames = 0;
    UInt32 abfSize = sizeof(actualBufferFrames);
    AudioObjectGetPropertyData(device, &ba, 0, NULL, &abfSize, &actualBufferFrames);
    printf("buffer frames: requested=%u actual=%u\n", bufferFrames, actualBufferFrames);
    state.framesPerCallback = actualBufferFrames ? actualBufferFrames : 192;

    for (int i = 0; i < 4; i++) {
        int16_t packet[kDtsBlockBytes / 4 * 2];
        if (readBlock(&state, packet) != 0) return 3;
        memcpy(&state.ring.data[i * (kDtsBlockBytes / 4) * 2], packet, kDtsBlockBytes);
        atomic_fetch_add(&state.ring.writeIndex, kDtsBlockBytes / 4);
    }

    AudioDeviceIOProcID proc = NULL;
    status = AudioDeviceCreateIOProcID(device, bitstreamIOProc, &state, &proc);
    if (status != noErr) fail("create IOProc", status);
    status = AudioDeviceStart(device, proc);
    if (status != noErr) fail("start device", status);

    printf("bitstream started: %.1f s, underruns=%llu\n", seconds,
           (unsigned long long)state.underruns);
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    uint64_t blockTicks = (uint64_t)((512.0 * 1e9 / kRate) * tb.denom / tb.numer);
    uint64_t next = mach_absolute_time();
    uint64_t end = next + (uint64_t)(seconds * 1e9 * tb.denom / tb.numer);
    while (mach_absolute_time() < end) {
        int16_t packet[kDtsBlockBytes / 4 * 2];
        if (readBlock(&state, packet) != 0) {
            rewind(state.input);
            if (readBlock(&state, packet) != 0) break;
        }
        unsigned wr = atomic_load(&state.ring.writeIndex);
        unsigned rd = atomic_load(&state.ring.readIndex);
        while (wr - rd > kRingFrames - (kDtsBlockBytes / 4)) {
            usleep(2000);
            rd = atomic_load(&state.ring.readIndex);
        }
        memcpy(&state.ring.data[wr & kRingMask], packet, kDtsBlockBytes);
        atomic_store(&state.ring.writeIndex, wr + kDtsBlockBytes / 4);
        next += blockTicks;
        mach_wait_until(next);
    }

    AudioDeviceStop(device, proc);
    AudioDeviceDestroyIOProcID(device, proc);
    AudioObjectSetPropertyData(stream, &va, 0, NULL, sizeof(oldFmt), &oldFmt);
    AudioObjectSetPropertyData(stream, &pa, 0, NULL, sizeof(oldFmt), &oldFmt);
    AudioObjectSetPropertyData(device, &ma, 0, NULL, sizeof(oldMix), &oldMix);
    AudioObjectSetPropertyData(device, &ha, 0, NULL, sizeof(oldHog), &oldHog);
    printf("bitstream stopped: underruns=%llu calls=%llu frames=%llu copied=%llu "
           "w=%u r=%u\n",
           (unsigned long long)state.underruns,
           (unsigned long long)atomic_load(&state.ioCalls),
           (unsigned long long)atomic_load(&state.ioFrames),
           (unsigned long long)atomic_load(&state.ioCopied),
           atomic_load(&state.ring.writeIndex), atomic_load(&state.ring.readIndex));
    printf("first IO buffer: buffers=%d bytes=%u channels=%u\n",
           atomic_load(&state.firstBuffers), atomic_load(&state.firstBytes),
           atomic_load(&state.firstChannels));
    fclose(state.input);
    return state.underruns ? 4 : 0;
}
