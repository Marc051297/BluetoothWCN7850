// QcaBluetooth: loads the RAM patch and NVM into a Qualcomm WCN785x/WCN6855 Bluetooth USB controller
// at device match time, so that bluetoothd (with BlueToolFixup) finds a working HCI controller.
#ifndef QCABLUETOOTH_HPP
#define QCABLUETOOTH_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <kern/thread_call.h>

struct qca_version {
    uint32_t rom_version;
    uint32_t patch_version;
    uint32_t ram_version;
    uint8_t  chip_id;
    uint8_t  platform_id;
    uint16_t flag;
    uint8_t  reserved[4];
} __attribute__((packed));

struct qca_device_info {
    uint32_t rom_version;
    uint8_t  rampatch_hdr;   // bytes of the rampatch file sent as the DFU header
    uint8_t  nvm_hdr;        // same for the NVM file
    uint8_t  ver_offset;     // offset of qca_rampatch_version inside the rampatch file
    const char *name;
};

class QcaBluetoothFirmware : public IOService {
    OSDeclareDefaultStructors(QcaBluetoothFirmware)

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn setPowerState(unsigned long state, IOService *whatDevice) override;

private:
    IOUSBHostDevice *mDevice = nullptr;
    IOUSBHostInterface *mInterface = nullptr;
    IOUSBHostPipe *mBulkOut = nullptr;
    IOBufferMemoryDescriptor *mBuf = nullptr;
    thread_call_t mResumeCall = nullptr;
    bool mWasOff = false;

    IOReturn vendorIn(uint8_t request, void *buf, uint16_t len);
    IOReturn readVersion(qca_version &ver, uint8_t &status);
    bool openBulk();
    void closeBulk();
    IOReturn download(const uint8_t *fw, size_t len, size_t hdr);
    IOReturn setup(bool &loadedSomething);
    void loadAndPublish(const char *why);
    static void resumeThread(thread_call_param_t owner, thread_call_param_t);
};

#endif
