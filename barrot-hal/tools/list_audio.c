#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size);
    int count = (int)(size / sizeof(AudioDeviceID));
    AudioDeviceID *ids = calloc(count ? (size_t)count : 1, sizeof(AudioDeviceID));
    if (!ids) return 1;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, ids);

    for (int i = 0; i < count; i++) {
        CFStringRef uid = NULL;
        CFStringRef name = NULL;
        UInt32 s = sizeof(uid);
        AudioObjectPropertyAddress ua = {
            kAudioDevicePropertyDeviceUID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioObjectPropertyAddress na = {
            kAudioObjectPropertyName,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(ids[i], &ua, 0, NULL, &s, &uid) != noErr || !uid)
            continue;
        char uidText[256] = "";
        CFStringGetCString(uid, uidText, sizeof(uidText), kCFStringEncodingUTF8);
        s = sizeof(name);
        if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &s, &name) == noErr && name) {
            char nameText[256] = "";
            CFStringGetCString(name, nameText, sizeof(nameText), kCFStringEncodingUTF8);
            printf("%u\t%s\t%s\n", ids[i], nameText, uidText);
            CFRelease(name);
        } else {
            printf("%u\t?\t%s\n", ids[i], uidText);
        }
        CFRelease(uid);
    }
    free(ids);
    return 0;
}
