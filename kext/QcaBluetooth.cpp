#include "QcaBluetooth.hpp"
#include "FwData.h"
#include <IOKit/IOLib.h>
#include <IOKit/usb/StandardUSB.h>
#include <libkern/version.h>
#include <pexpert/pexpert.h>

#define LOG(fmt, ...) log(fmt, ##__VA_ARGS__)

#define QCA_GET_TARGET_VERSION 0x09
#define QCA_CHECK_STATUS       0x05
#define QCA_DFU_DOWNLOAD       0x01
#define QCA_SYSCFG_UPDATED     0x40
#define QCA_PATCH_UPDATED      0x80
#define QCA_DFU_PACKET_LEN     4096
#define QCA_CTRL_BUF_LEN       64
#define QCA_DFU_TIMEOUT_MS     3000
#define QCA_CTRL_TIMEOUT_MS    5000
#define QCA_FLAG_MULTI_NVM     0x80
#define QCA_BT_RESET_WAIT_MS   100
#define WCN6855_2_0_RAM_VERSION_GF 0x400c1200
#define WCN6855_2_1_RAM_VERSION_GF 0x400c1211

// Same table as Linux btusb.c qca_devices_table.
static const qca_device_info kDevices[] = {
    { 0x00000100, 20, 4,  8, "Rome 1.0" },  { 0x00000101, 20, 4,  8, "Rome 1.1" },
    { 0x00000200, 28, 4, 16, "Rome 2.0" },  { 0x00000201, 28, 4, 16, "Rome 2.1" },
    { 0x00000300, 28, 4, 16, "Rome 3.0" },  { 0x00000302, 28, 4, 16, "Rome 3.2" },
    { 0x00130100, 40, 4, 16, "WCN6855 1.0" }, { 0x00130200, 40, 4, 16, "WCN6855 2.0" }, { 0x00130201, 40, 4, 16, "WCN6855 2.1" },
    { 0x00190200, 40, 4, 16, "WCN785x 2.0" },
};

static const qca_device_info *lookupDevice(uint32_t rom) {
    for (size_t i = 0; i < sizeof kDevices / sizeof kDevices[0]; i++)
        if (kDevices[i].rom_version == rom) return &kDevices[i];
    return nullptr;
}

static const qca_fw_entry *lookupFirmware(const char *name) {
    for (int i = 0; i < qca_fw_count; i++)
        if (strcmp(qca_fw_table[i].name, name) == 0) return &qca_fw_table[i];
    return nullptr;
}

static uint16_t boardIdOf(const qca_version &v) {
    uint16_t b = 0;
    if (((v.flag >> 8) & 0xff) == QCA_FLAG_MULTI_NVM) b = (uint16_t)((v.chip_id << 8) | v.platform_id);
    return b == 0xffff ? 0 : b;
}

#define super IOService
OSDefineMetaClassAndStructors(QcaBluetoothFirmware, IOService)

static IOPMPowerState kPowerStates[2] = {
    { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};

void QcaBluetoothFirmware::log(const char *fmt, ...) {
    char line[200];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    __builtin_va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof line) n = sizeof line - 1;
    IOLog("QcaBluetooth: %s\n", line);
    if (mLogLen + (size_t)n + 2 > sizeof mLog) return;   // ring is full: the first lines are the interesting ones
    memcpy(mLog + mLogLen, line, (size_t)n);
    mLogLen += (size_t)n;
    mLog[mLogLen++] = '\n';
    mLog[mLogLen] = 0;
    setProperty("Log", mLog);
}

void QcaBluetoothFirmware::stage(uint32_t n) { setProperty("Stage", n, 32); }

IOService *QcaBluetoothFirmware::probe(IOService *provider, SInt32 *score) {
    uint32_t off = 0;
    if (PE_parse_boot_argn("-qcabtoff", &off, sizeof off)) { IOLog("QcaBluetooth: disabled by -qcabtoff\n"); return nullptr; }
    if (!super::probe(provider, score)) return nullptr;
    IOUSBHostDevice *dev = OSDynamicCast(IOUSBHostDevice, provider);
    if (!dev) return nullptr;
    const StandardUSB::DeviceDescriptor *dd = dev->getDeviceDescriptor();
    if (!dd) { IOLog("QcaBluetooth: probe: no device descriptor\n"); return nullptr; }
    IOLog("QcaBluetooth: probe %04x:%04x bcdDevice %04x class %02x/%02x/%02x\n", USBToHost16(dd->idVendor), USBToHost16(dd->idProduct),
          USBToHost16(dd->bcdDevice), dd->bDeviceClass, dd->bDeviceSubClass, dd->bDeviceProtocol);
    return this;
}

bool QcaBluetoothFirmware::start(IOService *provider) {
    mDevice = OSDynamicCast(IOUSBHostDevice, provider);
    if (!mDevice) return false;
    if (!super::start(provider)) return false;
    setProperty("VersionInfo", QCABT_VERSION);
    PE_parse_boot_argn("qcabt_stage", &mStageLimit, sizeof mStageLimit);
    setProperty("StageLimit", mStageLimit, 32);
    stage(0);
    LOG("start %s, stage limit %u", QCABT_VERSION, mStageLimit);

    // Everything setPowerState may touch exists before power management can call it.
    mResumeCall = thread_call_allocate(resumeThread, this);
    if (!mResumeCall) { LOG("thread_call_allocate failed"); return false; }

    // Be a node in the power tree under the USB device, so that we see sleep/wake and can reload afterwards.
    PMinit();
    registerPowerDriver(this, kPowerStates, 2);
    provider->joinPMtree(this);
    makeUsable();

    // Everything happens synchronously here, before the device is reported to userspace as matched:
    // bluetoothd's first-match notification only fires once kernel matching (this start()) is done.
    loadAndPublish("match");
    registerService();
    return true;
}

void QcaBluetoothFirmware::stop(IOService *provider) {
    mStopping = true;
    if (mResumeCall) { thread_call_cancel_wait(mResumeCall); thread_call_free(mResumeCall); mResumeCall = nullptr; }
    closeBulk();
    closeCtrl();
    if (mDevice && mDevice->isOpen(this)) mDevice->close(this);
    PMstop();
    super::stop(provider);
}

IOReturn QcaBluetoothFirmware::setPowerState(unsigned long state, IOService *) {
    if (state == 0) mWasOff = true;
    else if (mWasOff) {
        mWasOff = false;
        // The chip keeps its RAM across a USB reset but not across a power loss; check from a thread,
        // setPowerState must not block on USB traffic.
        if (mResumeCall && !mStopping) {
            retain();                                        // released by resumeThread
            if (thread_call_enter(mResumeCall)) release();   // was already queued: drop the extra reference
        }
    }
    return kIOPMAckImplied;
}

void QcaBluetoothFirmware::resumeThread(thread_call_param_t owner, thread_call_param_t) {
    QcaBluetoothFirmware *me = static_cast<QcaBluetoothFirmware *>(owner);
    if (!me->mStopping && !me->isInactive() && me->mDevice && !me->mDevice->isInactive()) me->loadAndPublish("wake");
    me->release();
}

// Open the device, run the download if the status bits say it is needed, publish the result, close the device.
void QcaBluetoothFirmware::loadAndPublish(const char *why) {
    if (!mDevice || mDevice->isInactive()) { LOG("%s: device is gone", why); return; }
    setProperty("LastAttempt", why);
    if (mStageLimit < kStageOpen) { LOG("%s: parked before stage %u", why, kStageOpen); return; }
    if (!mDevice->open(this)) {
        // Normal after boot: bluetoothd holds the device (USBDeviceOpenSeize). Nothing we can do from here.
        LOG("%s: cannot open the device (held by another client), not touching it", why);
        return;
    }
    stage(kStageOpen);
    bool loaded = false;
    IOReturn ret = openCtrl() ? setup(loaded) : kIOReturnNoMemory;
    closeBulk();
    closeCtrl();
    mDevice->close(this);
    setProperty("LastResult", (uint64_t)(uint32_t)ret, 32);
    if (ret == kIOReturnSuccess) {
        mDevice->setProperty("FirmwareLoaded", true);
        // What IntelBluetoothFirmware does on Monterey+: the name bluetoothd shows for a USB controller.
        if (version_major >= 21) mDevice->setName("Bluetooth USB Host Controller");
        LOG("%s: firmware %s", why, loaded ? "loaded" : "already present");
    } else if (ret == kIOReturnNotReady) {
        LOG("%s: parked by qcabt_stage=%u", why, mStageLimit);
    } else {
        mDevice->setProperty("FirmwareLoaded", false);
        LOG("%s: firmware load failed: 0x%08x", why, ret);
    }
}

bool QcaBluetoothFirmware::openCtrl() {
    mCtrl = mDevice->createIOBuffer(kIODirectionInOut, QCA_CTRL_BUF_LEN);
    if (!mCtrl) { LOG("cannot allocate the control buffer"); return false; }
    if (mCtrl->prepare() != kIOReturnSuccess) { LOG("cannot prepare the control buffer"); OSSafeReleaseNULL(mCtrl); return false; }
    return true;
}

void QcaBluetoothFirmware::closeCtrl() {
    if (mCtrl) { mCtrl->complete(); OSSafeReleaseNULL(mCtrl); }
}

IOReturn QcaBluetoothFirmware::vendorIn(uint8_t request, void *buf, uint16_t len) {
    if (len > QCA_CTRL_BUF_LEN) return kIOReturnBadArgument;
    StandardUSB::DeviceRequest rq;
    rq.bmRequestType = makeDeviceRequestbmRequestType(kRequestDirectionIn, kRequestTypeVendor, kRequestRecipientDevice);
    rq.bRequest = request; rq.wValue = 0; rq.wIndex = 0; rq.wLength = len;
    uint32_t done = 0;
    IOReturn ret = mDevice->deviceRequest(this, rq, mCtrl, done, QCA_CTRL_TIMEOUT_MS);
    if (ret == kIOReturnSuccess && done != len) ret = kIOReturnUnderrun;
    if (ret == kIOReturnSuccess) memcpy(buf, mCtrl->getBytesNoCopy(), len);
    return ret;
}

IOReturn QcaBluetoothFirmware::readVersion(qca_version &ver, uint8_t &status) {
    IOReturn ret = vendorIn(QCA_GET_TARGET_VERSION, &ver, sizeof ver);
    if (ret) { LOG("GET_TARGET_VERSION failed: 0x%08x", ret); return ret; }
    ret = vendorIn(QCA_CHECK_STATUS, &status, 1);
    if (ret) { LOG("CHECK_STATUS failed: 0x%08x", ret); return ret; }
    ver.rom_version = OSSwapLittleToHostInt32(ver.rom_version);
    ver.patch_version = OSSwapLittleToHostInt32(ver.patch_version);
    ver.ram_version = OSSwapLittleToHostInt32(ver.ram_version);
    ver.flag = OSSwapLittleToHostInt16(ver.flag);
    return kIOReturnSuccess;
}

// Configure the device and open interface 0 so bulk OUT endpoint 2 is reachable.
bool QcaBluetoothFirmware::openBulk() {
    if (mBulkOut) return true;
    const StandardUSB::ConfigurationDescriptor *cd = mDevice->getConfigurationDescriptor((uint8_t)0);
    if (!cd) { LOG("no configuration descriptor"); return false; }
    // Unconditional, as IntelBluetoothFirmware does: we hold the device exclusively, so nobody else's configuration is lost.
    IOReturn ret = mDevice->setConfiguration(cd->bConfigurationValue, true);
    if (ret) { LOG("setConfiguration(%u) failed: 0x%08x", cd->bConfigurationValue, ret); return false; }
    OSIterator *it = mDevice->getChildIterator(gIOServicePlane);
    if (!it) { LOG("no child iterator"); return false; }
    OSObject *o;
    while ((o = it->getNextObject())) {
        IOUSBHostInterface *intf = OSDynamicCast(IOUSBHostInterface, o);
        if (intf && intf->getInterfaceDescriptor() && intf->getInterfaceDescriptor()->bInterfaceNumber == 0) { intf->retain(); mInterface = intf; break; }
    }
    it->release();
    if (!mInterface) { LOG("interface 0 not found"); return false; }
    if (!mInterface->open(this)) { LOG("cannot open interface 0"); OSSafeReleaseNULL(mInterface); return false; }
    mBulkOut = mInterface->copyPipe(0x02);
    if (!mBulkOut) { LOG("bulk OUT endpoint 2 not found"); return false; }
    mBuf = mDevice->createIOBuffer(kIODirectionOut, QCA_DFU_PACKET_LEN);
    if (!mBuf) { LOG("cannot allocate the transfer buffer"); return false; }
    if (mBuf->prepare(kIODirectionOut) != kIOReturnSuccess) { LOG("cannot prepare the transfer buffer"); OSSafeReleaseNULL(mBuf); return false; }
    return true;
}

void QcaBluetoothFirmware::closeBulk() {
    if (mBuf) { mBuf->complete(kIODirectionOut); OSSafeReleaseNULL(mBuf); }
    if (mBulkOut) { mBulkOut->abort(); OSSafeReleaseNULL(mBulkOut); }
    if (mInterface) { if (mInterface->isOpen(this)) mInterface->close(this); OSSafeReleaseNULL(mInterface); }
}

// Linux btusb_setup_qca_download_fw: header on EP0, body on bulk OUT 2 in 4 KiB writes.
IOReturn QcaBluetoothFirmware::download(const uint8_t *fw, size_t len, size_t hdr) {
    size_t size = len < hdr ? len : hdr;
    if (size > QCA_CTRL_BUF_LEN) return kIOReturnBadArgument;
    memcpy(mCtrl->getBytesNoCopy(), fw, size);
    StandardUSB::DeviceRequest rq;
    rq.bmRequestType = makeDeviceRequestbmRequestType(kRequestDirectionOut, kRequestTypeVendor, kRequestRecipientDevice);
    rq.bRequest = QCA_DFU_DOWNLOAD; rq.wValue = 0; rq.wIndex = 0; rq.wLength = (uint16_t)size;
    uint32_t done = 0;
    IOReturn ret = mDevice->deviceRequest(this, rq, mCtrl, done, QCA_CTRL_TIMEOUT_MS);
    if (ret) { LOG("DFU_DOWNLOAD header failed: 0x%08x", ret); return ret; }
    size_t sent = size, count = len - size;
    IOSleep(20);   // ep2 needs time to switch from ACL to DFU mode (Linux comment)
    uint8_t *buf = (uint8_t *)mBuf->getBytesNoCopy();
    while (count) {
        size = count < QCA_DFU_PACKET_LEN ? count : QCA_DFU_PACKET_LEN;
        memcpy(buf, fw + sent, size);
        done = 0;
        ret = mBulkOut->io(mBuf, (uint32_t)size, done, QCA_DFU_TIMEOUT_MS);
        if (ret) { LOG("bulk write failed at %lu of %lu: 0x%08x", (unsigned long)sent, (unsigned long)len, ret); return ret; }
        if (done != size) { LOG("short bulk write at %lu: %u of %lu", (unsigned long)sent, done, (unsigned long)size); return kIOReturnUnderrun; }
        sent += size; count -= size;
    }
    return kIOReturnSuccess;
}

// Linux btusb_setup_qca.
IOReturn QcaBluetoothFirmware::setup(bool &loaded) {
    qca_version ver; uint8_t status; char name[64];
    loaded = false;
    IOReturn ret = readVersion(ver, status);
    if (ret) return ret;
    const qca_device_info *info = lookupDevice(ver.rom_version);
    LOG("rom 0x%08x (%s) patch 0x%08x ram 0x%08x chip %02x platform %02x flag 0x%04x board 0x%04x status 0x%02x",
        ver.rom_version, info ? info->name : "unknown", ver.patch_version, ver.ram_version, ver.chip_id, ver.platform_id, ver.flag, boardIdOf(ver), status);
    setProperty("RomVersion", ver.rom_version, 32);
    setProperty("PatchVersion", ver.patch_version, 32);
    setProperty("Status", status, 8);
    if (!info) {
        if (ver.rom_version & ~0xffffU) { LOG("unknown high ROM version, assuming no patch is needed"); return kIOReturnSuccess; }
        LOG("unsupported ROM version");
        return kIOReturnUnsupported;
    }

    if (!(status & QCA_PATCH_UPDATED)) {
        snprintf(name, sizeof name, "rampatch_usb_%08x.bin", ver.rom_version);
        const qca_fw_entry *fw = lookupFirmware(name);
        if (!fw) { LOG("no embedded %s", name); return kIOReturnNotFound; }
        const uint8_t *rv = fw->data + info->ver_offset;
        uint16_t romHi = (uint16_t)(rv[0] | rv[1] << 8), romLo = (uint16_t)(rv[2] | rv[3] << 8), patch = (uint16_t)(rv[4] | rv[5] << 8);
        uint32_t rom = (ver.rom_version & ~0xffffU) ? ((uint32_t)romHi << 16 | romLo) : romLo;
        LOG("%s: rom 0x%08x build 0x%04x (controller build 0x%08x)", name, rom, patch, ver.patch_version);
        if (rom != ver.rom_version || patch < ver.patch_version) { LOG("rampatch does not match the controller"); return kIOReturnBadArgument; }
        if (mStageLimit < kStageConfigure) return kIOReturnNotReady;
        stage(kStageConfigure);
        if (!openBulk()) return kIOReturnNoResources;
        if (mStageLimit < kStageRampatch) return kIOReturnNotReady;
        stage(kStageRampatch);
        ret = download(fw->data, fw->size, info->rampatch_hdr);
        if (ret) return ret;
        loaded = true;
        ret = readVersion(ver, status);
        if (ret) return ret;
        LOG("after rampatch: patch 0x%08x status 0x%02x", ver.patch_version, status);
        setProperty("PatchVersion", ver.patch_version, 32);
        setProperty("Status", status, 8);
        setProperty("RampatchFile", name);
    }

    if (!(status & QCA_SYSCFG_UPDATED)) {
        const char *variant = (ver.ram_version == WCN6855_2_0_RAM_VERSION_GF || ver.ram_version == WCN6855_2_1_RAM_VERSION_GF) ? "_gf" : "";
        uint16_t board = boardIdOf(ver);
        if (board) snprintf(name, sizeof name, "nvm_usb_%08x%s_%04x.bin", ver.rom_version, variant, board);
        else       snprintf(name, sizeof name, "nvm_usb_%08x%s.bin", ver.rom_version, variant);
        const qca_fw_entry *fw = lookupFirmware(name);
        if (!fw) { LOG("no embedded %s", name); return kIOReturnNotFound; }
        if (mStageLimit < kStageConfigure) return kIOReturnNotReady;
        stage(kStageConfigure);
        if (!openBulk()) return kIOReturnNoResources;
        if (mStageLimit < kStageNvm) return kIOReturnNotReady;
        stage(kStageNvm);
        ret = download(fw->data, fw->size, info->nvm_hdr);
        if (ret) return ret;
        loaded = true;
        if (info->rom_version >= 0x00130201) IOSleep(QCA_BT_RESET_WAIT_MS);   // the controller resets to apply the NVM
        ret = readVersion(ver, status);
        if (ret) return ret;
        LOG("after nvm: patch 0x%08x status 0x%02x", ver.patch_version, status);
        setProperty("Status", status, 8);
        setProperty("NvmFile", name);
    }
    if ((status & (QCA_PATCH_UPDATED | QCA_SYSCFG_UPDATED)) != (QCA_PATCH_UPDATED | QCA_SYSCFG_UPDATED)) {
        LOG("status 0x%02x after the download: the controller did not take the firmware", status);
        return kIOReturnDeviceError;
    }
    stage(100);
    return kIOReturnSuccess;
}
