/*
 * Loads the built .driver bundle in-process and exercises the plug-in object
 * model without involving coreaudiod. This bypasses AMFI's system HAL load
 * policy, so it validates driver logic even on machines without a signing
 * certificate.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define kDeviceID 0x100u
#define kStreamID 0x101u

typedef struct {
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
} Stats;

typedef struct {
    AudioServerPlugInDriverRef drv;
    atomic_bool stop;
    double sampleRate;
} Producer;

static void *producerThread(void *arg) {
    Producer *p = (Producer *)arg;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    uint64_t ticksPerBlock = (uint64_t)((double)512 * 1e9 / p->sampleRate *
                                        tb.denom / tb.numer);
    uint64_t nextTick = mach_absolute_time();
    Float32 block[512 * 2];
    double phase = 0;
    while (!atomic_load(&p->stop)) {
        for (int i = 0; i < 512; i++) {
            float v = 0.25f * sinf((float)phase);
            phase += 2 * M_PI * 440.0 / p->sampleRate;
            block[i * 2] = v;
            block[i * 2 + 1] = v;
        }
        (*p->drv)->DoIOOperation(p->drv, kDeviceID, kStreamID, 1,
                                 kAudioServerPlugInIOOperationWriteMix, 512, NULL,
                                 block, NULL);
        nextTick += ticksPerBlock;
        mach_wait_until(nextTick);
    }
    return NULL;
}

static int getData(AudioServerPlugInDriverRef drv, AudioObjectID obj,
                   AudioObjectPropertySelector sel, AudioObjectPropertyScope scope,
                   UInt32 *outSize, void *out, UInt32 qualSize, const void *qual) {
    AudioObjectPropertyAddress a = {sel, scope, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    OSStatus r = (*drv)->GetPropertyDataSize(drv, obj, 0, &a, qualSize, qual, &size);
    if (r) { printf("  size sel=%c%c%c%c failed %d\n",
                    (char)(sel >> 24), (char)(sel >> 16), (char)(sel >> 8), (char)sel, (int)r);
             return 0; }
    if (size > *outSize) { printf("  buffer too small for sel=%c%c%c%c\n",
                                 (char)(sel >> 24), (char)(sel >> 16),
                                 (char)(sel >> 8), (char)sel); return 0; }
    r = (*drv)->GetPropertyData(drv, obj, 0, &a, qualSize, qual, size, outSize, out);
    if (r) { printf("  data sel=%c%c%c%c failed %d\n",
                    (char)(sel >> 24), (char)(sel >> 16), (char)(sel >> 8), (char)sel, (int)r);
             return 0; }
    return 1;
}

static void printCF(AudioServerPlugInDriverRef drv, AudioObjectID obj,
                    AudioObjectPropertySelector sel) {
    CFStringRef s = NULL; UInt32 size = sizeof(s);
    if (getData(drv, obj, sel, kAudioObjectPropertyScopeGlobal, &size, &s, 0, NULL) && s) {
        char b[256] = "";
        CFStringGetCString(s, b, sizeof(b), kCFStringEncodingUTF8);
        printf("  %s\n", b);
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *path = argc > 1 ? argv[1] :
        "build/RH899X.driver/Contents/MacOS/RH899X";
    double seconds = argc > 2 ? atof(argv[2]) : 2.0;
    double rate = argc > 3 ? atof(argv[3]) : 48000.0;
    int runs = argc > 4 ? atoi(argv[4]) : 2;
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    void *(*create)(CFAllocatorRef, CFUUIDRef) =
        (void *(*)(CFAllocatorRef, CFUUIDRef))dlsym(h, "Ranhe_CreatePlugin");
    if (!create) { printf("factory missing\n"); return 1; }
    void *drvRaw = create(kCFAllocatorDefault, kAudioServerPlugInTypeUUID);
    if (!drvRaw) { printf("factory returned NULL\n"); return 1; }
    AudioServerPlugInDriverRef drv = (AudioServerPlugInDriverRef)drvRaw;
    OSStatus r = (*drv)->Initialize(drv, NULL);
    printf("Initialize: %d\n", (int)r);
    if (r) return 1;

    AudioObjectID dev = 0; UInt32 size = sizeof(dev);
    if (!getData(drv, kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList,
                 kAudioObjectPropertyScopeGlobal, &size, &dev, 0, NULL)) return 1;
    printf("device id: %u (expected %u)\n", dev, kDeviceID);
    printf("device name:"); printCF(drv, kDeviceID, kAudioObjectPropertyName);
    printf("device uid:"); printCF(drv, kDeviceID, kAudioDevicePropertyDeviceUID);
    printf("transport: "); {
        UInt32 v = 0; size = sizeof(v);
        getData(drv, kDeviceID, kAudioDevicePropertyTransportType,
                kAudioObjectPropertyScopeGlobal, &size, &v, 0, NULL);
        printf("0x%x\n", v);
    }
    printf("stream config: "); {
        AudioBufferList bl; memset(&bl, 0, sizeof(bl)); size = sizeof(bl);
        getData(drv, kDeviceID, kAudioDevicePropertyStreamConfiguration,
                kAudioObjectPropertyScopeOutput, &size, &bl, 0, NULL);
        printf("%u buffers, %u channels\n", bl.mNumberBuffers,
               bl.mNumberBuffers ? bl.mBuffers[0].mNumberChannels : 0);
    }
    Float64 nominalRate = rate;
    AudioObjectPropertyAddress rateAddr = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    r = (*drv)->SetPropertyData(drv, kDeviceID, 0, &rateAddr, 0, NULL,
                                sizeof(nominalRate), &nominalRate);
    printf("set nominal rate %.0f: %d\n", rate, (int)r);
    if (r) return 1;
    printf("asbd: "); {
        AudioStreamBasicDescription asbd; memset(&asbd, 0, sizeof(asbd));
        size = sizeof(asbd);
        getData(drv, kStreamID, kAudioStreamPropertyVirtualFormat,
                kAudioObjectPropertyScopeGlobal, &size, &asbd, 0, NULL);
        printf("%.0f Hz, %u ch, %u bits, flags=0x%x\n", asbd.mSampleRate,
               asbd.mChannelsPerFrame, asbd.mBitsPerChannel, asbd.mFormatFlags);
    }

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    for (int run = 0; run < runs; run++) {
        (*drv)->StartIO(drv, kDeviceID, 1);
        Producer prod;
        prod.drv = drv;
        prod.sampleRate = rate;
        atomic_init(&prod.stop, false);
        pthread_t ptid;
        pthread_create(&ptid, NULL, producerThread, &prod);
        Stats prev; memset(&prev, 0, sizeof(prev));
        uint64_t startTick = mach_absolute_time();
        double duration = run == 0 ? seconds : 1.0;
        int lastPrint = -1;
        while (1) {
            uint64_t now = mach_absolute_time();
            double elapsed = (double)(now - startTick) * tb.numer / tb.denom / 1e9;
            if (elapsed >= duration) break;
            int sec = (int)elapsed;
            if (sec >= lastPrint + 5) {
                lastPrint = sec;
                Stats st; memset(&st, 0, sizeof(st)); size = sizeof(st);
                if (getData(drv, kDeviceID, 'rhst', kAudioObjectPropertyScopeGlobal,
                            &size, &st, 0, NULL)) {
                    printf("run=%d sec=%d tx=+%llu frames=+%llu under=+%llu "
                           "drop=+%llu dup=+%llu level=%u usb_err=%llu frame_err=%llu\n",
                           run, sec,
                           (unsigned long long)(st.usbTransfers - prev.usbTransfers),
                           (unsigned long long)(st.usbFrames - prev.usbFrames),
                           (unsigned long long)(st.underruns - prev.underruns),
                           (unsigned long long)(st.drops - prev.drops),
                           (unsigned long long)(st.dups - prev.dups),
                           st.ringLevel,
                           (unsigned long long)st.usbErrors,
                           (unsigned long long)st.frameErrors);
                    prev = st;
                }
            }
            usleep(200000);
        }
        printf("run=%d duration %.3f s\n", run, duration);
        atomic_store(&prod.stop, true);
        pthread_join(ptid, NULL);
        (*drv)->StopIO(drv, kDeviceID, 1);
        usleep(200000);
    }
    return 0;
}
