// qcabt: userspace firmware loader / inspector for Qualcomm WCN6855/WCN7850 (and Rome) Bluetooth USB controllers.
// Reimplements the Linux btusb.c btusb_setup_qca() sequence with IOUSBLib:
//   vendor IN  0x09  GET_TARGET_VERSION  -> 20-byte version block
//   vendor IN  0x05  CHECK_STATUS        -> status byte (0x80 patch loaded, 0x40 NVM loaded)
//   vendor OUT 0x01  DFU_DOWNLOAD        -> firmware header, then the body on bulk OUT endpoint 2
//
//   qcabt info [vid pid]
//   qcabt load [-d firmware-dir] [-n] [vid pid]     (-n: dry run, print the plan only)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USBSpec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define QCA_GET_TARGET_VERSION 0x09
#define QCA_CHECK_STATUS       0x05
#define QCA_DFU_DOWNLOAD       0x01
#define QCA_SYSCFG_UPDATED     0x40
#define QCA_PATCH_UPDATED      0x80
#define QCA_DFU_PACKET_LEN     4096
#define QCA_DFU_TIMEOUT_MS     3000
#define QCA_FLAG_MULTI_NVM     0x80
#define WCN6855_2_0_RAM_VERSION_GF 0x400c1200
#define WCN6855_2_1_RAM_VERSION_GF 0x400c1211

struct qca_version { uint32_t rom_version, patch_version, ram_version; uint8_t chip_id, platform_id; uint16_t flag; uint8_t reserved[4]; } __attribute__((packed));
struct qca_device_info { uint32_t rom_version; uint8_t rampatch_hdr, nvm_hdr, ver_offset; const char *name; };
static const struct qca_device_info qca_devices_table[] = {
    { 0x00000100, 20, 4,  8, "Rome 1.0" }, { 0x00000101, 20, 4,  8, "Rome 1.1" },
    { 0x00000200, 28, 4, 16, "Rome 2.0" }, { 0x00000201, 28, 4, 16, "Rome 2.1" },
    { 0x00000300, 28, 4, 16, "Rome 3.0" }, { 0x00000302, 28, 4, 16, "Rome 3.2" },
    { 0x00130100, 40, 4, 16, "WCN6855 1.0" }, { 0x00130200, 40, 4, 16, "WCN6855 2.0" }, { 0x00130201, 40, 4, 16, "WCN6855 2.1" },
    { 0x00190200, 40, 4, 16, "WCN785x 2.0" },
};

static IOUSBDeviceInterface650 **gDev;
static IOUSBInterfaceInterface800 **gIntf;
static UInt8 gBulkOutPipe;

static int die(const char *what, kern_return_t kr) { fprintf(stderr, "ERROR: %s: 0x%08x\n", what, kr); return 1; }

static io_service_t find_device(long vid, long pid) {
    CFMutableDictionaryRef m = IOServiceMatching(kIOUSBDeviceClassName);
    CFNumberRef v = CFNumberCreate(0, kCFNumberLongType, &vid), p = CFNumberCreate(0, kCFNumberLongType, &pid);
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), v); CFDictionarySetValue(m, CFSTR(kUSBProductID), p);
    CFRelease(v); CFRelease(p);
    return IOServiceGetMatchingService(kIOMainPortDefault, m);
}

static int open_device(long vid, long pid, int seize) {
    io_service_t svc = find_device(vid, pid);
    if (!svc) { fprintf(stderr, "ERROR: no USB device %04lx:%04lx\n", vid, pid); return 1; }
    IOCFPlugInInterface **plug = 0; SInt32 score;
    kern_return_t kr = IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(svc);
    if (kr) return die("IOCreatePlugInInterfaceForService(device)", kr);
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650), (LPVOID *)&gDev);
    (*plug)->Release(plug);
    kr = seize ? (*gDev)->USBDeviceOpenSeize(gDev) : (*gDev)->USBDeviceOpen(gDev);
    if (kr) return die(seize ? "USBDeviceOpenSeize" : "USBDeviceOpen (is bluetoothd holding it? try -s)", kr);
    return 0;
}

static void close_device(void) {
    if (gIntf) { (*gIntf)->USBInterfaceClose(gIntf); (*gIntf)->Release(gIntf); gIntf = 0; }
    if (gDev) { (*gDev)->USBDeviceClose(gDev); (*gDev)->Release(gDev); gDev = 0; }
}

static kern_return_t vendor_in(UInt8 req, void *buf, UInt16 len) {
    IOUSBDevRequestTO rq = { USBmakebmRequestType(kUSBIn, kUSBVendor, kUSBDevice), req, 0, 0, len, buf, 0, 5000, 5000 };
    return (*gDev)->DeviceRequestTO(gDev, &rq);
}

static int read_version(struct qca_version *ver, uint8_t *status) {
    kern_return_t kr = vendor_in(QCA_GET_TARGET_VERSION, ver, sizeof *ver);
    if (kr) return die("GET_TARGET_VERSION", kr);
    kr = vendor_in(QCA_CHECK_STATUS, status, 1);
    if (kr) return die("CHECK_STATUS", kr);
    return 0;
}

static uint16_t board_id_of(const struct qca_version *v) {
    uint16_t b = 0;
    if (((v->flag >> 8) & 0xff) == QCA_FLAG_MULTI_NVM) b = (v->chip_id << 8) | v->platform_id;
    return b == 0xffff ? 0 : b;
}

static const struct qca_device_info *lookup(uint32_t rom) {
    for (size_t i = 0; i < sizeof qca_devices_table / sizeof *qca_devices_table; i++)
        if (qca_devices_table[i].rom_version == rom) return &qca_devices_table[i];
    return 0;
}

static void print_version(const char *tag, const struct qca_version *v, uint8_t st) {
    const struct qca_device_info *info = lookup(v->rom_version);
    printf("%s rom_version=0x%08x (%s) patch_version=0x%08x ram_version=0x%08x chip_id=0x%02x platform_id=0x%02x flag=0x%04x board_id=0x%04x\n",
           tag, v->rom_version, info ? info->name : "unknown", v->patch_version, v->ram_version, v->chip_id, v->platform_id, v->flag, board_id_of(v));
    printf("%s status=0x%02x rampatch=%s nvm=%s\n", tag, st, st & QCA_PATCH_UPDATED ? "loaded" : "NOT loaded", st & QCA_SYSCFG_UPDATED ? "loaded" : "NOT loaded");
}

static void nvm_name(char *out, size_t n, const struct qca_version *v) {
    const char *variant = (v->ram_version == WCN6855_2_0_RAM_VERSION_GF || v->ram_version == WCN6855_2_1_RAM_VERSION_GF) ? "_gf" : "";
    uint16_t b = board_id_of(v);
    if (b) snprintf(out, n, "nvm_usb_%08x%s_%04x.bin", v->rom_version, variant, b);
    else   snprintf(out, n, "nvm_usb_%08x%s.bin", v->rom_version, variant);
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); return 0; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n); if (fread(b, 1, n, f) != (size_t)n) { perror(path); free(b); fclose(f); return 0; }
    fclose(f); *len = n; return b;
}

// Configure the device and open interface 0 so bulk OUT endpoint 2 is reachable.
static int open_bulk(void) {
    UInt8 cur = 0; kern_return_t kr = (*gDev)->GetConfiguration(gDev, &cur);
    if (kr) return die("GetConfiguration", kr);
    if (cur == 0) {
        IOUSBConfigurationDescriptorPtr cd;
        kr = (*gDev)->GetConfigurationDescriptorPtr(gDev, 0, &cd); if (kr) return die("GetConfigurationDescriptorPtr", kr);
        kr = (*gDev)->SetConfiguration(gDev, cd->bConfigurationValue); if (kr) return die("SetConfiguration", kr);
        printf("set configuration %u\n", cd->bConfigurationValue);
    }
    IOUSBFindInterfaceRequest fr = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
    io_iterator_t it; kr = (*gDev)->CreateInterfaceIterator(gDev, &fr, &it); if (kr) return die("CreateInterfaceIterator", kr);
    io_service_t isvc;
    while ((isvc = IOIteratorNext(it))) {
        IOCFPlugInInterface **plug = 0; SInt32 score;
        kr = IOCreatePlugInInterfaceForService(isvc, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
        IOObjectRelease(isvc);
        if (kr) continue;
        IOUSBInterfaceInterface800 **intf = 0;
        (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID800), (LPVOID *)&intf); (*plug)->Release(plug);
        UInt8 num = 0xff; (*intf)->GetInterfaceNumber(intf, &num);
        if (num != 0) { (*intf)->Release(intf); continue; }
        kr = (*intf)->USBInterfaceOpen(intf); if (kr) { (*intf)->Release(intf); IOObjectRelease(it); return die("USBInterfaceOpen(0)", kr); }
        UInt8 n = 0; (*intf)->GetNumEndpoints(intf, &n);
        for (UInt8 p = 1; p <= n; p++) {
            UInt8 dir, number, type, interval; UInt16 mps;
            if ((*intf)->GetPipeProperties(intf, p, &dir, &number, &type, &mps, &interval)) continue;
            if (dir == kUSBOut && type == kUSBBulk && number == 2) gBulkOutPipe = p;
        }
        gIntf = intf; break;
    }
    IOObjectRelease(it);
    if (!gIntf) { fprintf(stderr, "ERROR: interface 0 not found\n"); return 1; }
    if (!gBulkOutPipe) { fprintf(stderr, "ERROR: bulk OUT endpoint 2 not found on interface 0\n"); return 1; }
    return 0;
}

static int download(const uint8_t *fw, size_t len, size_t hdr) {
    size_t size = len < hdr ? len : hdr;
    uint8_t *buf = malloc(QCA_DFU_PACKET_LEN);
    memcpy(buf, fw, size);
    IOUSBDevRequestTO rq = { USBmakebmRequestType(kUSBOut, kUSBVendor, kUSBDevice), QCA_DFU_DOWNLOAD, 0, 0, (UInt16)size, buf, 0, 5000, 5000 };
    kern_return_t kr = (*gDev)->DeviceRequestTO(gDev, &rq);
    if (kr) { free(buf); return die("DFU_DOWNLOAD header", kr); }
    size_t sent = size, count = len - size;
    usleep(20000);  // Linux: "ep2 need time to switch from function acl to function dfu"
    while (count) {
        size = count < QCA_DFU_PACKET_LEN ? count : QCA_DFU_PACKET_LEN;
        memcpy(buf, fw + sent, size);
        kr = (*gIntf)->WritePipeTO(gIntf, gBulkOutPipe, buf, (UInt32)size, QCA_DFU_TIMEOUT_MS, QCA_DFU_TIMEOUT_MS);
        if (kr) { fprintf(stderr, "ERROR: bulk write at %zu of %zu: 0x%08x\n", sent, len, kr); free(buf); return 1; }
        sent += size; count -= size;
    }
    free(buf);
    printf("downloaded %zu bytes (%zu header + %zu body)\n", len, hdr, len - hdr);
    return 0;
}

static int cmd_load(const char *dir, int dry, long vid, long pid, int seize) {
    struct qca_version ver; uint8_t st; char path[1024], name[80];
    if (open_device(vid, pid, seize)) return 1;
    if (read_version(&ver, &st)) return 1;
    print_version("before:", &ver, st);
    const struct qca_device_info *info = lookup(ver.rom_version);
    if (!info) {
        if (ver.rom_version & ~0xffffU) { printf("high ROM version with no table entry: assuming no patch needed (as Linux does)\n"); return 0; }
        fprintf(stderr, "ERROR: unsupported ROM version 0x%08x\n", ver.rom_version); return 1;
    }
    if (!(st & QCA_PATCH_UPDATED)) {
        snprintf(path, sizeof path, "%s/rampatch_usb_%08x.bin", dir, ver.rom_version);
        size_t len; uint8_t *fw = read_file(path, &len); if (!fw) return 1;
        const uint8_t *rv = fw + info->ver_offset;
        uint16_t rom_hi = rv[0] | rv[1] << 8, rom_lo = rv[2] | rv[3] << 8, patch = rv[4] | rv[5] << 8;
        uint32_t rom = (ver.rom_version & ~0xffffU) ? (uint32_t)rom_hi << 16 | rom_lo : rom_lo;
        printf("rampatch %s: file rom 0x%08x build 0x%04x; controller rom 0x%08x build 0x%08x\n", path, rom, patch, ver.rom_version, ver.patch_version);
        if (rom != ver.rom_version || patch < ver.patch_version) { fprintf(stderr, "ERROR: rampatch file does not match the controller\n"); return 1; }
        if (!dry) {
            if (open_bulk()) return 1;
            if (download(fw, len, info->rampatch_hdr)) return 1;
            if (read_version(&ver, &st)) return 1;
            print_version("after rampatch:", &ver, st);
        }
        free(fw);
    } else printf("rampatch already loaded, skipping\n");
    if (!(st & QCA_SYSCFG_UPDATED)) {
        nvm_name(name, sizeof name, &ver);
        snprintf(path, sizeof path, "%s/%s", dir, name);
        size_t len; uint8_t *fw = read_file(path, &len); if (!fw) return 1;
        printf("nvm %s: %zu bytes\n", path, len);
        if (!dry) {
            if (!gIntf && open_bulk()) return 1;
            if (download(fw, len, info->nvm_hdr)) return 1;
            if (info->rom_version >= 0x00130201) usleep(100000);  // controller resets itself to apply the NVM
            // The reset may drop our handle; reopen the device to read the result.
            close_device();
            for (int i = 0; i < 30; i++) { usleep(100000); if (find_device(vid, pid)) break; }
            if (open_device(vid, pid, seize)) return 1;
            if (read_version(&ver, &st)) return 1;
            print_version("after nvm:", &ver, st);
        }
        free(fw);
    } else printf("nvm already loaded, skipping\n");
    close_device();
    return 0;
}

int main(int argc, char **argv) {
    const char *dir = "firmware/dl"; int dry = 0, seize = 0, c;
    while ((c = getopt(argc, argv, "d:ns")) != -1) { if (c == 'd') dir = optarg; else if (c == 'n') dry = 1; else if (c == 's') seize = 1; else return 2; }
    if (optind >= argc) { fprintf(stderr, "usage: qcabt [-d fwdir] [-n] [-s] info|load [vid pid]\n"); return 2; }
    const char *cmd = argv[optind++];
    long vid = optind < argc ? strtol(argv[optind++], 0, 16) : 0x0489, pid = optind < argc ? strtol(argv[optind++], 0, 16) : 0xe10a;
    if (!strcmp(cmd, "info")) {
        struct qca_version ver; uint8_t st;
        if (open_device(vid, pid, seize) || read_version(&ver, &st)) return 1;
        print_version("", &ver, st);
        char n[80]; nvm_name(n, sizeof n, &ver);
        printf(" firmware files: rampatch_usb_%08x.bin, %s\n", ver.rom_version, n);
        close_device(); return 0;
    }
    if (!strcmp(cmd, "load")) return cmd_load(dir, dry, vid, pid, seize);
    fprintf(stderr, "unknown command %s\n", cmd); return 2;
}
