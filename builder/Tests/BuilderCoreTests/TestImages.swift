import CryptoKit
import Foundation

/// Tiny ISO9660 images for the tests (never a real game file): `files` maps "DIR/NAME" to bytes.
func makeImage(_ files: [String: Data], raw mode: UInt8? = nil) -> Data {
    func record(_ name: [UInt8], _ extent: UInt32, _ size: UInt32, _ directory: Bool) -> [UInt8] {
        let length = 33 + name.count + (1 - name.count % 2)
        var r = [UInt8](repeating: 0, count: length)
        r[0] = UInt8(length)
        func put(_ at: Int, _ v: UInt32) {
            for i in 0..<4 { r[at + i] = UInt8((v >> (8 * UInt32(i))) & 0xff); r[at + 7 - i] = UInt8((v >> (8 * UInt32(i))) & 0xff) }
        }
        put(2, extent)
        put(10, size)
        r[25] = directory ? 2 : 0
        r[28] = 1; r[31] = 1
        r[32] = UInt8(name.count)
        r.replaceSubrange(33..<(33 + name.count), with: name)
        return r
    }
    var folders: [String: [(String, Data)]] = [:]
    for (path, data) in files.sorted(by: { $0.key < $1.key }) {
        let parts = path.split(separator: "/").map(String.init)
        folders[parts[0], default: []].append((parts[1], data))
    }
    let names = folders.keys.sorted()
    var sectors: [Int: [UInt8]] = [:]
    let root: UInt32 = 18
    var next = 19 + UInt32(names.count)
    var rootList = record([0], root, 2048, true) + record([1], root, 2048, true)
    for (index, folder) in names.enumerated() {
        let extent = 19 + UInt32(index)
        rootList += record(Array(folder.utf8), extent, 2048, true)
        var list = record([0], extent, 2048, true) + record([1], root, 2048, true)
        for (name, data) in folders[folder]! {
            list += record(Array((name + ";1").utf8), next, UInt32(data.count), false)
            var offset = 0
            repeat {
                var chunk = [UInt8](data[offset..<min(data.count, offset + 2048)])
                chunk += [UInt8](repeating: 0, count: 2048 - chunk.count)
                sectors[Int(next)] = chunk
                next += 1
                offset += 2048
            } while offset < data.count
        }
        sectors[Int(extent)] = list + [UInt8](repeating: 0, count: 2048 - list.count)
    }
    sectors[Int(root)] = rootList + [UInt8](repeating: 0, count: 2048 - rootList.count)
    var pvd = [UInt8](repeating: 0, count: 2048)
    pvd[0] = 1
    pvd.replaceSubrange(1..<6, with: Array("CD001".utf8))
    pvd[6] = 1
    pvd.replaceSubrange(156..<190, with: record([0], root, 2048, true))
    sectors[16] = pvd
    var out = Data()
    let sync: [UInt8] = [0] + [UInt8](repeating: 0xff, count: 10) + [0]
    for index in 0...sectors.keys.max()! {
        let sector = sectors[index] ?? [UInt8](repeating: 0, count: 2048)
        switch mode {
        case nil: out += sector
        case 1?: out += sync + [0, 0, 0, 1] + sector + [UInt8](repeating: 0, count: 288)
        default: out += sync + [0, 0, 0, 2] + [UInt8](repeating: 0, count: 8) + sector + [UInt8](repeating: 0, count: 280)
        }
    }
    return out
}

func sha(_ data: Data) -> String {
    SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
}

func temporaryFolder() -> URL {
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("gearhead-tests-\(UUID().uuidString)")
    try! FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    return url
}
