// pair-mouse [name-filter]
// Connects to the first Bluetooth LE HID device (mouse/keyboard/gamepad) that appears while scanning,
// immediately, so a device that rotates its address in pairing mode cannot slip away. Once the link is up,
// macOS's BTLEServer claims the HID service and pairs it; reading the Report Map is a second pairing trigger.
import CoreBluetooth
import Foundation

let hidService = CBUUID(string: "1812")
let reportMap = CBUUID(string: "2A4B")
let filter = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : nil

func log(_ s: String) {
    let t = DateFormatter.localizedString(from: Date(), dateStyle: .none, timeStyle: .medium)
    print("[\(t)] \(s)")
    fflush(stdout)
}

final class Pairer: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var central: CBCentralManager!
    var target: CBPeripheral?
    var seen = Set<UUID>()

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    func startScan() {
        target = nil
        log("Scanning for a Bluetooth LE HID device" + (filter.map { " whose name contains \"\($0)\"" } ?? "") + " - put it in pairing mode now.")
        central.scanForPeripherals(withServices: [hidService], options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
    }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        switch c.state {
        case .poweredOn: startScan()
        case .unauthorized:
            log("Terminal is not allowed to use Bluetooth. Allow it in System Settings > Privacy & Security > Bluetooth, then run again.")
            exit(2)
        case .poweredOff: log("Bluetooth is off.")
        default: log("Bluetooth state \(c.state.rawValue), waiting...")
        }
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral, advertisementData ad: [String: Any], rssi: NSNumber) {
        guard target == nil else { return }
        let name = (ad[CBAdvertisementDataLocalNameKey] as? String) ?? p.name ?? "(no name)"
        if seen.insert(p.identifier).inserted {
            log("Seen: \(name)  RSSI \(rssi)  id \(p.identifier.uuidString.prefix(8))")
        }
        if let f = filter, !name.localizedCaseInsensitiveContains(f) { return }
        if let connectable = ad[CBAdvertisementDataIsConnectable] as? Bool, !connectable { return }
        log("Connecting to \(name) ...")
        target = p
        p.delegate = self
        c.stopScan()
        c.connect(p, options: nil)
        DispatchQueue.main.asyncAfter(deadline: .now() + 10) { [weak self] in
            guard let self, let t = self.target, t.identifier == p.identifier, t.state != .connected else { return }
            log("No connection after 10 s (the device may have changed address) - scanning again.")
            c.cancelPeripheralConnection(t)
            self.startScan()
        }
    }

    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        log("Connected. macOS should now claim the HID service and pair; also reading the Report Map to trigger pairing.")
        p.discoverServices(nil)
        DispatchQueue.main.asyncAfter(deadline: .now() + 25) {
            log("Done waiting. Check System Settings > Bluetooth: the device should be listed as Connected. Exiting.")
            exit(0)
        }
    }

    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        log("Connection failed: \(error?.localizedDescription ?? "unknown") - scanning again.")
        startScan()
    }

    func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        log("Disconnected: \(error?.localizedDescription ?? "no error")")
    }

    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        let services = p.services ?? []
        log("Services: " + (services.isEmpty ? "none visible to this tool (normal for HID on macOS)" : services.map { $0.uuid.uuidString }.joined(separator: ", ")))
        for s in services where s.uuid == hidService {
            p.discoverCharacteristics([reportMap], for: s)
        }
    }

    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
        for ch in s.characteristics ?? [] where ch.uuid == reportMap {
            p.readValue(for: ch)
        }
    }

    func peripheral(_ p: CBPeripheral, didUpdateValueFor ch: CBCharacteristic, error: Error?) {
        if let error {
            log("Report Map read: \(error.localizedDescription)")
        } else {
            log("Report Map read OK (\(ch.value?.count ?? 0) bytes): the link is encrypted, so pairing worked.")
        }
    }
}

let pairer = Pairer()
DispatchQueue.main.asyncAfter(deadline: .now() + 180) {
    log("Gave up after 3 minutes.")
    exit(1)
}
dispatchMain()
