#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    double phase;
    double amplitude;
    double sampleRate;
} RenderState;

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
    OSStatus status = AudioObjectGetPropertyDataSize(
        kAudioObjectSystemObject, &addr, 0, NULL, &size);
    if (status != noErr) fail("list devices", status);

    AudioDeviceID *devices = calloc(1, size);
    if (!devices) fail("allocate device list", kAudioHardwareUnspecifiedError);
    status = AudioObjectGetPropertyData(
        kAudioObjectSystemObject, &addr, 0, NULL, &size, devices);
    if (status != noErr) fail("read device list", status);

    AudioDeviceID found = kAudioObjectUnknown;
    for (UInt32 i = 0; i < size / sizeof(AudioDeviceID); i++) {
        AudioObjectPropertyAddress uidAddr = {
            kAudioDevicePropertyDeviceUID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        CFStringRef uid = NULL;
        UInt32 uidSize = sizeof(uid);
        status = AudioObjectGetPropertyData(
            devices[i], &uidAddr, 0, NULL, &uidSize, &uid);
        if (status == noErr && uid) {
            if (CFEqual(uid, wantedUID)) found = devices[i];
            CFRelease(uid);
        }
        if (found != kAudioObjectUnknown) break;
    }
    free(devices);
    return found;
}

static void printDevice(AudioDeviceID device) {
    AudioObjectPropertyAddress nameAddr = {
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    CFStringRef name = NULL;
    UInt32 size = sizeof(name);
    OSStatus status = AudioObjectGetPropertyData(
        device, &nameAddr, 0, NULL, &size, &name);
    if (status == noErr && name) {
        char text[256] = "";
        CFStringGetCString(name, text, sizeof(text), kCFStringEncodingUTF8);
        printf("device: %u %s\n", (unsigned)device, text);
        CFRelease(name);
    }

    Float64 rate = 0;
    AudioObjectPropertyAddress rateAddr = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    size = sizeof(rate);
    if (AudioObjectGetPropertyData(device, &rateAddr, 0, NULL, &size, &rate) == noErr)
        printf("sample rate: %.0f Hz\n", rate);

    UInt32 transport = 0;
    AudioObjectPropertyAddress transportAddr = {
        kAudioDevicePropertyTransportType,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    size = sizeof(transport);
    if (AudioObjectGetPropertyData(device, &transportAddr, 0, NULL, &size, &transport) == noErr)
        printf("transport: 0x%x\n", (unsigned)transport);
}

static UInt32 isRunning(AudioDeviceID device) {
    UInt32 running = 0;
    UInt32 size = sizeof(running);
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyDeviceIsRunningSomewhere,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(device, &addr, 0, NULL, &size, &running) != noErr)
        return 0;
    return running;
}

static OSStatus renderTone(void *refCon, AudioUnitRenderActionFlags *flags,
                           const AudioTimeStamp *timestamp, UInt32 bus,
                           UInt32 frames, AudioBufferList *data) {
    (void)flags;
    (void)timestamp;
    (void)bus;
    RenderState *state = (RenderState *)refCon;
    const double step = 2.0 * M_PI * 440.0 / state->sampleRate;

    for (UInt32 b = 0; b < data->mNumberBuffers; b++) {
        AudioBuffer *buf = &data->mBuffers[b];
        Float32 *samples = (Float32 *)buf->mData;
        UInt32 channels = buf->mNumberChannels;
        for (UInt32 frame = 0; frame < frames; frame++) {
            Float32 value = (Float32)(state->amplitude * sin(state->phase));
            state->phase += step;
            for (UInt32 channel = 0; channel < channels; channel++)
                samples[frame * channels + channel] = value;
        }
    }
    if (state->phase > 2.0 * M_PI)
        state->phase = fmod(state->phase, 2.0 * M_PI);
    return noErr;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    double seconds = argc > 1 ? atof(argv[1]) : 5.0;
    double rate = argc > 2 ? atof(argv[2]) : 48000.0;
    if (seconds < 1.0) seconds = 1.0;

    const char *uidText = "RH899X-0c1c-3a1d";
    CFStringRef uid = CFStringCreateWithCString(
        kCFAllocatorDefault, uidText, kCFStringEncodingUTF8);
    AudioDeviceID device = findDevice(uid);
    CFRelease(uid);
    if (device == kAudioObjectUnknown) {
        fprintf(stderr, "device UID not found: %s\n", uidText);
        return 2;
    }
    printDevice(device);

    Float64 requestedRate = rate;
    AudioObjectPropertyAddress rateAddr = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    OSStatus status = AudioObjectSetPropertyData(
        device, &rateAddr, 0, NULL, sizeof(requestedRate), &requestedRate);
    if (status != noErr) fail("set nominal sample rate", status);
    Float64 actualRate = 0;
    UInt32 actualRateSize = sizeof(actualRate);
    status = AudioObjectGetPropertyData(
        device, &rateAddr, 0, NULL, &actualRateSize, &actualRate);
    if (status != noErr) fail("read nominal sample rate", status);
    printf("requested rate: %.0f Hz, actual: %.0f Hz\n", rate, actualRate);

    UInt32 framesPerBuffer = 512;
    AudioObjectPropertyAddress bufferAddr = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    status = AudioObjectSetPropertyData(
        device, &bufferAddr, 0, NULL, sizeof(framesPerBuffer), &framesPerBuffer);
    if (status != noErr) fail("set buffer frame size", status);

    AudioComponentDescription desc = {
        kAudioUnitType_Output,
        kAudioUnitSubType_HALOutput,
        kAudioUnitManufacturer_Apple,
        0,
        0
    };
    AudioComponent component = AudioComponentFindNext(NULL, &desc);
    if (!component) fail("find HAL output unit", kAudioUnitErr_InvalidProperty);
    AudioUnit unit = NULL;
    status = AudioComponentInstanceNew(component, &unit);
    if (status != noErr) fail("create HAL output unit", status);

    UInt32 enableOutput = 1;
    status = AudioUnitSetProperty(
        unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0,
        &enableOutput, sizeof(enableOutput));
    if (status != noErr) fail("enable audio output", status);

    status = AudioUnitSetProperty(
        unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
        &device, sizeof(device));
    if (status != noErr) fail("select RH-899X", status);

    AudioStreamBasicDescription format;
    memset(&format, 0, sizeof(format));
    format.mSampleRate = actualRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = 2 * sizeof(Float32);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 2 * sizeof(Float32);
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 32;
    status = AudioUnitSetProperty(
        unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
        &format, sizeof(format));
    if (status != noErr) fail("set client stream format", status);

    RenderState renderState = {0.0, 0.25, actualRate};
    AURenderCallbackStruct callback = {renderTone, &renderState};
    status = AudioUnitSetProperty(
        unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
        &callback, sizeof(callback));
    if (status != noErr) fail("set render callback", status);

    status = AudioUnitInitialize(unit);
    if (status != noErr) fail("initialize audio unit", status);

    for (int run = 0; run < 2; run++) {
        double duration = run == 0 ? seconds : 2.0;
        printf("run %d: start\n", run + 1);
        status = AudioOutputUnitStart(unit);
        if (status != noErr) fail("start audio unit", status);

        for (int second = 1; second <= (int)duration; second++) {
            sleep(1);
            printf("  run %d second %d running=%u\n",
                   run + 1, second, (unsigned)isRunning(device));
        }

        status = AudioOutputUnitStop(unit);
        if (status != noErr) fail("stop audio unit", status);
        printf("run %d: stopped running=%u\n", run + 1, (unsigned)isRunning(device));
        if (run == 0) sleep(1);
    }

    status = AudioUnitUninitialize(unit);
    if (status != noErr) fail("uninitialize audio unit", status);
    AudioComponentInstanceDispose(unit);

    printf("final running=%u\n", (unsigned)isRunning(device));
    return 0;
}
