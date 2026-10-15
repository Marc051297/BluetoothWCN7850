// usbdesc: dump the device and configuration descriptors of a USB device by VID/PID via IOUSBLib.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    long vid = argc > 1 ? strtol(argv[1], 0, 16) : 0x0489;
    long pid = argc > 2 ? strtol(argv[2], 0, 16) : 0xe10a;
    CFMutableDictionaryRef m = IOServiceMatching(kIOUSBDeviceClassName);
    CFNumberRef v = CFNumberCreate(0, kCFNumberLongType, &vid), p = CFNumberCreate(0, kCFNumberLongType, &pid);
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), v); CFDictionarySetValue(m, CFSTR(kUSBProductID), p);
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, m);
    if (!svc) { fprintf(stderr, "no device %04lx:%04lx\n", vid, pid); return 1; }
    IOCFPlugInInterface **plug = 0; SInt32 score;
    kern_return_t kr = IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    if (kr) { fprintf(stderr, "plugin: 0x%x\n", kr); return 1; }
    IOUSBDeviceInterface650 **dev = 0;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650), (LPVOID *)&dev);
    (*plug)->Release(plug);
    UInt16 vendor, product, rel; UInt8 cls, sub, proto, nconf, speed;
    (*dev)->GetDeviceVendor(dev, &vendor); (*dev)->GetDeviceProduct(dev, &product); (*dev)->GetDeviceReleaseNumber(dev, &rel);
    (*dev)->GetDeviceClass(dev, &cls); (*dev)->GetDeviceSubClass(dev, &sub); (*dev)->GetDeviceProtocol(dev, &proto);
    (*dev)->GetNumberOfConfigurations(dev, &nconf); (*dev)->GetDeviceSpeed(dev, &speed);
    printf("Device %04x:%04x bcdDevice=%04x class=%02x/%02x/%02x speed=%u configs=%u\n", vendor, product, rel, cls, sub, proto, speed, nconf);
    for (UInt8 c = 0; c < nconf; c++) {
        IOUSBConfigurationDescriptorPtr cd;
        if ((*dev)->GetConfigurationDescriptorPtr(dev, c, &cd)) continue;
        printf("Configuration %u: bConfigurationValue=%u bNumInterfaces=%u bmAttributes=0x%02x MaxPower=%umA wTotalLength=%u\n",
               c, cd->bConfigurationValue, cd->bNumInterfaces, cd->bmAttributes, cd->MaxPower * 2, USBToHostWord(cd->wTotalLength));
        const UInt8 *q = (const UInt8 *)cd, *end = q + USBToHostWord(cd->wTotalLength);
        q += cd->bLength;
        while (q + 2 <= end && q[0] >= 2) {
            if (q[1] == kUSBInterfaceDesc && q[0] >= 9)
                printf("  Interface %u alt %u: class=%02x/%02x/%02x endpoints=%u\n", q[2], q[3], q[5], q[6], q[7], q[4]);
            else if (q[1] == kUSBEndpointDesc && q[0] >= 7) {
                UInt8 addr = q[2], attr = q[3]; UInt16 mps = q[4] | (q[5] << 8);
                static const char *types[] = {"control", "isoc", "bulk", "interrupt"};
                printf("    EP 0x%02x %s %-9s maxpacket=%u interval=%u\n", addr, (addr & 0x80) ? "IN " : "OUT", types[attr & 3], mps, q[6]);
            } else printf("  descriptor type 0x%02x len %u\n", q[1], q[0]);
            q += q[0];
        }
        printf("  raw:"); for (const UInt8 *r = (const UInt8 *)cd; r < end; r++) printf(" %02x", *r); printf("\n");
    }
    UInt8 cur = 0; kr = (*dev)->GetConfiguration(dev, &cur); printf("Current configuration: %u (kr=0x%x)\n", cur, kr);
    (*dev)->Release(dev); IOObjectRelease(svc);
    return 0;
}
