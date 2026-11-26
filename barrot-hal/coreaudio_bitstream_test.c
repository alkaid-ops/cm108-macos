#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define kRingFrames 65536u
#define kRingMask (kRingFrames - 1u)
#define kRate 48000u
#define kChannels 2u
#define kMaxPacketBytes 8192u

typedef struct {
    int16_t data[kRingFrames * kChannels];
    atomic_uint writeIndex;
    atomic_uint readIndex;
} Ring;

typedef struct {
    Ring ring;
    FILE *input;
    atomic_ullong underruns;
    atomic_ullong ioCalls;
    atomic_ullong ioFrames;
    atomic_ullong ioCopied;
    atomic_int firstBuffers;
    atomic_uint firstBytes;
    atomic_uint firstChannels;
    UInt32 framesPerCallback;
} TestState;

static const uint16_t kAc3Words48k[38] = {
    64, 64, 80, 80, 96, 96, 112, 112, 128, 128, 160, 160,
    192, 192, 224, 224, 256, 256, 320, 320, 384, 384,
    448, 448, 512, 512, 640, 640, 768, 768, 896, 896,
    1024, 1024, 1152, 1152, 1280, 1280
};

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

static uint16_t rd_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)((p[1] << 8) | p[0]);
}

static uint32_t rd_be24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static uint32_t rd_le24(const uint8_t *p) {
    return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static int isSync(const uint8_t *p, int *isDts, int *swap) {
    if (p[0] == 0x0b && p[1] == 0x77) {
        *isDts = 0;
        *swap = 1;
        return 1;
    }
    if (memcmp(p, "\x7f\xfe\x80\x01", 4) == 0) {
        *isDts = 1;
        *swap = 1;
        return 1;
    }
    if (memcmp(p, "\xfe\x7f\x01\x80", 4) == 0) {
        *isDts = 1;
        *swap = 0;
        return 1;
    }
    return 0;
}

static int findSync(TestState *s, uint8_t *frame, int *isDts, int *swap) {
    if (fread(frame, 1, 4, s->input) != 4) return 1;
    for (unsigned tries = 0; tries < kMaxPacketBytes; tries++) {
        if (isSync(frame, isDts, swap)) return 0;
        memmove(frame, frame + 1, 3);
        if (fread(frame + 3, 1, 1, s->input) != 1) return 1;
    }
    return 2;
}

static int readBlock(TestState *s, uint8_t *pkt, unsigned *outFrames, const char **fmt) {
    uint8_t frame[kMaxPacketBytes];
    int isDts = 0;
    int swap = 0;
    if (findSync(s, frame, &isDts, &swap)) return 1;
    if (fread(frame + 4, 1, 4, s->input) != 4) return 1;

    unsigned frameBytes = 0;
    unsigned pktBytes = 0;
    unsigned dataType = 0;

    if (!isDts) {
        unsigned fscod = (frame[4] >> 6) & 0x3;
        unsigned frmsizecod = frame[4] & 0x3f;
        if (fscod != 0 || frmsizecod >= 38) {
            fprintf(stderr, "AC3 frame is not 48 kHz or has bad frmsizecod\n");
            return 3;
        }
        frameBytes = kAc3Words48k[frmsizecod] * 2;
        if (frameBytes < 8 || frameBytes > kMaxPacketBytes) return 3;
        if (fread(frame + 8, 1, frameBytes - 8, s->input) != frameBytes - 8) return 1;
        pktBytes = 6144;
        dataType = 0x01 | ((frame[5] & 0x7) << 8);
        *fmt = "AC3";
    } else {
        uint16_t b16 = swap ? rd_be16(frame + 4) : rd_le16(frame + 4);
        uint32_t b24 = swap ? rd_be24(frame + 5) : rd_le24(frame + 5);
        unsigned blocks = ((b16 >> 2) & 0x7f) + 1;
        unsigned coreSize = ((b24 >> 4) & 0x3fff) + 1;
        unsigned samples = blocks << 5;
        if (samples != 512 && samples != 1024 && samples != 2048) return 3;
        if (coreSize < 8 || coreSize > kMaxPacketBytes) return 3;
        if (fread(frame + 8, 1, coreSize - 8, s->input) != coreSize - 8) return 1;
        frameBytes = coreSize;
        pktBytes = samples * 4;
        dataType = samples == 512 ? 0x0b : (samples == 1024 ? 0x0c : 0x0d);
        *fmt = "DTS";
    }

    if (pktBytes < 8 || pktBytes > kMaxPacketBytes) return 3;
    memset(pkt, 0, pktBytes);
    pkt[0] = 0x72; pkt[1] = 0xf8;              /* Pa */
    pkt[2] = 0x1f; pkt[3] = 0x4e;              /* Pb */
    pkt[4] = dataType & 0xff;                  /* Pc */
    pkt[5] = (dataType >> 8) & 0xff;
    unsigned lengthBits = ((frameBytes + 1) & ~1u) << 3;
    pkt[6] = lengthBits & 0xff;                /* Pd */
    pkt[7] = (lengthBits >> 8) & 0xff;

    for (unsigned i = 0; i < frameBytes; i += 2) {
        unsigned j = i + 1 < frameBytes ? i + 1 : i;
        if (swap) {
            pkt[8 + i] = frame[j];
            pkt[9 + i] = frame[i];
        } else {
            pkt[8 + i] = frame[i];
            pkt[9 + i] = frame[j];
        }
    }
    *outFrames = pktBytes / 4;
    return 0;
}

static void ringWrite(TestState *s, const uint8_t *pkt, unsigned frames) {
    unsigned wr = atomic_load(&s->ring.writeIndex);
    unsigned idx = wr & kRingMask;
    unsigned first = frames;
    if (idx + first > kRingFrames) first = kRingFrames - idx;
    memcpy(&s->ring.data[idx * kChannels], pkt, first * kChannels * sizeof(int16_t));
    if (first < frames)
        memcpy(&s->ring.data[0], pkt + first * kChannels * sizeof(int16_t),
               (frames - first) * kChannels * sizeof(int16_t));
    atomic_store(&s->ring.writeIndex, wr + frames);
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
        dst[i * kChannels] = s->ring.data[idx * kChannels];
        dst[i * kChannels + 1] = s->ring.data[idx * kChannels + 1];
    }
    if (copy < frames) {
        memset(dst + copy * kChannels, 0,
               (frames - copy) * kChannels * sizeof(int16_t));
        atomic_fetch_add(&s->underruns, frames - copy);
    }
    atomic_store(&s->ring.readIndex, rd + copy);
    return noErr;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s file.dts|file.ac3 [seconds]\n", argv[0]);
        return 2;
    }
    double seconds = argc > 2 ? atof(argv[2]) : 5.0;
    TestState state;
    memset(&state, 0, sizeof(state));
    atomic_init(&state.ring.writeIndex, 0);
    atomic_init(&state.ring.readIndex, 0);
    atomic_init(&state.underruns, 0);
    atomic_init(&state.ioCalls, 0);
    atomic_init(&state.ioFrames, 0);
    atomic_init(&state.ioCopied, 0);
    atomic_init(&state.firstBuffers, 0);
    atomic_init(&state.firstBytes, 0);
    atomic_init(&state.firstChannels, 0);
    state.framesPerCallback = 0;
    state.input = fopen(argv[1], "rb");
    if (!state.input) { perror(argv[1]); return 2; }

    CFStringRef uid = CFSTR("RH899X-0c1c-3a1d");
    AudioDeviceID device = findDevice(uid);
    if (device == kAudioObjectUnknown) {
        fprintf(stderr, "RH-899X not found\n");
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

    const char *fmtName = "?";
    uint8_t packet[kMaxPacketBytes];
    unsigned frames = 0;
    for (int i = 0; i < 4; i++) {
        int r = readBlock(&state, packet, &frames, &fmtName);
        if (r != 0) {
            fprintf(stderr, "cannot read first frame (%d)\n", r);
            return 3;
        }
        ringWrite(&state, packet, frames);
    }
    printf("input format: %s, packet frames=%u\n", fmtName, frames);

    AudioDeviceIOProcID proc = NULL;
    status = AudioDeviceCreateIOProcID(device, bitstreamIOProc, &state, &proc);
    if (status != noErr) fail("create IOProc", status);
    status = AudioDeviceStart(device, proc);
    if (status != noErr) fail("start device", status);

    printf("bitstream started: %.1f s, underruns=%llu\n", seconds,
           (unsigned long long)atomic_load(&state.underruns));
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    uint64_t next = mach_absolute_time();
    uint64_t end = next + (uint64_t)(seconds * 1e9 * tb.denom / tb.numer);
    while (mach_absolute_time() < end) {
        int r = readBlock(&state, packet, &frames, &fmtName);
        if (r != 0) {
            rewind(state.input);
            if (readBlock(&state, packet, &frames, &fmtName) != 0) break;
        }
        unsigned wr = atomic_load(&state.ring.writeIndex);
        unsigned rd = atomic_load(&state.ring.readIndex);
        while (wr - rd > kRingFrames - frames) {
            usleep(2000);
            rd = atomic_load(&state.ring.readIndex);
        }
        ringWrite(&state, packet, frames);
        next += (uint64_t)((double)frames * 1e9 / kRate * tb.denom / tb.numer);
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
           (unsigned long long)atomic_load(&state.underruns),
           (unsigned long long)atomic_load(&state.ioCalls),
           (unsigned long long)atomic_load(&state.ioFrames),
           (unsigned long long)atomic_load(&state.ioCopied),
           atomic_load(&state.ring.writeIndex), atomic_load(&state.ring.readIndex));
    printf("first IO buffer: buffers=%d bytes=%u channels=%u\n",
           atomic_load(&state.firstBuffers), atomic_load(&state.firstBytes),
           atomic_load(&state.firstChannels));
    fclose(state.input);
    return atomic_load(&state.underruns) ? 4 : 0;
}
