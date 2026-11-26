#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int number(io_service_t service, const char *key) {
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key,
                                              kCFStringEncodingUTF8);
    CFTypeRef v = IORegistryEntryCreateCFProperty(service, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    int n = -1;
    if (v) {
        if (CFGetTypeID(v) == CFNumberGetTypeID())
            CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &n);
        CFRelease(v);
    }
    return n;
}

static void done(void *refcon, IOReturn result, void *arg0) {
    (void)arg0;
    *(IOReturn *)refcon = result;
}

typedef struct {
    const char *name;
    unsigned entries;
    unsigned bytes;
    unsigned stride;
} Shape;

int main(int argc, char **argv) {
    int only = argc > 1 ? atoi(argv[1]) : -1;
    io_iterator_t iter = 0;
    IOServiceGetMatchingServices(kIOMainPortDefault,
                                 IOServiceMatching("IOUSBHostDevice"), &iter);
    io_service_t device = 0, s;
    while ((s = IOIteratorNext(iter))) {
        if (number(s, "idVendor") == 0x0c1c && number(s, "idProduct") == 0x3a1d) {
            device = s;
            break;
        }
        IOObjectRelease(s);
    }
    IOObjectRelease(iter);
    if (!device) { printf("device missing\n"); return 1; }

    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    IOCreatePlugInInterfaceForService(device, kIOUSBDeviceUserClientTypeID,
                                      kIOCFPlugInInterfaceID, &plugin, &score);
    IOUSBDeviceInterface **dev = NULL;
    (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
                              (LPVOID *)&dev);
    IOReturn r = (*dev)->USBDeviceOpenSeize(dev);
    printf("USBDeviceOpenSeize 0x%x\n", r);
    if (r) return 1;

    io_iterator_t itfIter = 0;
    IOUSBFindInterfaceRequest req = {kIOUSBFindInterfaceDontCare,
                                     kIOUSBFindInterfaceDontCare,
                                     kIOUSBFindInterfaceDontCare,
                                     kIOUSBFindInterfaceDontCare};
    (*dev)->CreateInterfaceIterator(dev, &req, &itfIter);
    io_service_t is;
    IOUSBInterfaceInterface **itf = NULL;
    while ((is = IOIteratorNext(itfIter))) {
        if (number(is, "bInterfaceNumber") == 1) {
            IOCFPlugInInterface **ip = NULL;
            IOCreatePlugInInterfaceForService(is, kIOUSBInterfaceUserClientTypeID,
                                              kIOCFPlugInInterfaceID, &ip, &score);
            (*ip)->QueryInterface(ip, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID),
                                  (LPVOID *)&itf);
            IOObjectRelease(is);
            break;
        }
        IOObjectRelease(is);
    }
    IOObjectRelease(itfIter);
    if (!itf) { printf("stream interface missing\n"); return 1; }
    r = (*itf)->USBInterfaceOpenSeize(itf);
    printf("USBInterfaceOpenSeize 0x%x\n", r);
    if (r) return 1;
    mach_port_t port = MACH_PORT_NULL;
    (*itf)->CreateInterfaceAsyncPort(itf, &port);
    CFRunLoopSourceRef src = NULL;
    (*itf)->CreateInterfaceAsyncEventSource(itf, &src);
    if (src) CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
    r = (*itf)->SetAlternateInterface(itf, 2);
    printf("alt 2 0x%x\n", r);
    if (r) return 1;

    UInt8 endpoints = 0;
    (*itf)->GetNumEndpoints(itf, &endpoints);
    for (UInt8 p = 1; p <= endpoints; p++) {
        UInt8 dir = 0, num = 0, type = 0, interval = 0;
        UInt16 maxpkt = 0;
        IOReturn pr = (*itf)->GetPipeProperties(itf, p, &dir, &num, &type,
                                                &maxpkt, &interval);
        printf("pipe %u rc=0x%x dir=%u ep=%u type=%u maxpkt=%u int=%u\n",
               p, pr, dir, num, type, maxpkt, interval);
    }

    static const Shape shapes[] = {
        {"64x192 stride8", 64, 192, 8},
        {"64x192 all", 64, 192, 1},
        {"8x192 all", 8, 192, 1},
        {"8x192 stride8", 8, 192, 8},
        {"64x24 all", 64, 24, 1},
        {"64x192 first", 64, 192, 64},
        {"8x192 first", 8, 192, 8},
    };

    UInt64 start = 0;
    AbsoluteTime now;
    (*itf)->GetBusFrameNumber(itf, &start, &now);
    start = (start + 8) & ~(UInt64)7;
    for (unsigned t = 0; t < sizeof(shapes) / sizeof(shapes[0]); t++) {
        if (only >= 0 && (int)t != only) continue;
        const Shape *sh = &shapes[t];
        IOUSBIsocFrame *frames = calloc(sh->entries, sizeof(*frames));
        unsigned total = 0;
        for (unsigned i = 0; i < sh->entries; i++) {
            frames[i].frReqCount = (i % sh->stride == 0) ? sh->bytes : 0;
            total += frames[i].frReqCount;
        }
        unsigned char *buf = calloc(1, total ? total : 1);
        IOReturn result = kIOReturnNoCompletion;
        r = (*itf)->WriteIsochPipeAsync(itf, 1, buf, start, sh->entries,
                                        frames, done, &result);
        printf("%-18s entries=%u bytes=%u stride=%u total=%u -> 0x%x\n",
               sh->name, sh->entries, sh->bytes, sh->stride, total, r);
        if (r == kIOReturnSuccess) {
            for (unsigned spin = 0; spin < 200 && result == kIOReturnNoCompletion; spin++)
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, false);
            printf("  completion 0x%x\n", result);
        }
        free(buf);
        free(frames);
        (*itf)->SetAlternateInterface(itf, 0);
        (*itf)->SetAlternateInterface(itf, 2);
    }

    (*itf)->SetAlternateInterface(itf, 0);
    (*itf)->USBInterfaceClose(itf);
    (*itf)->Release(itf);
    (*dev)->USBDeviceClose(dev);
    (*dev)->Release(dev);
    (*plugin)->Release(plugin);
    IOObjectRelease(device);
    return 0;
}
