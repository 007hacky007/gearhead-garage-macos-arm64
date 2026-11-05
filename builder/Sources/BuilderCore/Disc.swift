import CryptoKit
import Foundation

public struct DiscError: LocalizedError, Equatable {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var errorDescription: String? { message }
}

/// The BIN named by a CUE sheet: exactly one BINARY file beside it, first track data.
public func cueImage(_ cue: URL) throws -> URL {
    guard let data = try? Data(contentsOf: cue), data.count < 1 << 20,
          let text = String(data: data, encoding: .utf8) ?? String(data: data, encoding: .isoLatin1)
    else { throw DiscError("The CUE sheet cannot be read") }
    var files: [String] = []
    var firstTrack: String?
    for raw in text.components(separatedBy: .newlines) {
        let line = raw.trimmingCharacters(in: .whitespaces)
        let upper = line.uppercased()
        if upper.hasPrefix("FILE "), let open = line.firstIndex(of: "\""),
           let close = line[line.index(after: open)...].firstIndex(of: "\"") {
            files.append(String(line[line.index(after: open)..<close]))
        } else if upper.hasPrefix("TRACK "), firstTrack == nil {
            let parts = upper.split(separator: " ")
            if parts.count >= 3 { firstTrack = String(parts[2]) }
        }
    }
    guard files.count == 1 else { throw DiscError("The CUE sheet must name exactly one BIN file") }
    guard let track = firstTrack, ["MODE1/2352", "MODE2/2352", "MODE1/2048"].contains(track) else {
        throw DiscError("The first track of the CUE sheet is not a data track")
    }
    let name = files[0]
    guard !name.isEmpty, name != ".", name != "..", !name.contains("/"), !name.contains("\\"), !name.contains(":") else {
        throw DiscError("The CUE sheet's BIN must be a file beside it")
    }
    let image = cue.deletingLastPathComponent().appendingPathComponent(name)
    guard FileManager.default.fileExists(atPath: image.path) else {
        throw DiscError("\(name), named by the CUE sheet, is not beside it")
    }
    return image
}

/// Reads files from the ISO9660 file system of a CD image: 2048-byte sectors (.iso) or raw
/// 2352-byte MODE1 or MODE2 form 1 sectors (.bin, .img). Only reads.
public final class Disc {
    private let handle: FileHandle
    private let stride: UInt64
    private let offset: UInt64
    private var root: (extent: UInt32, size: UInt32) = (0, 0)

    public init(_ url: URL) throws {
        let image = url.pathExtension.lowercased() == "cue" ? try cueImage(url) : url
        guard let handle = try? FileHandle(forReadingFrom: image) else { throw DiscError("The disc image cannot be opened") }
        self.handle = handle
        handle.seek(toFileOffset: 16 * 2352)
        let raw = [UInt8](handle.readData(ofLength: 2352))
        let sync: [UInt8] = [0] + [UInt8](repeating: 0xff, count: 10) + [0]
        if raw.count == 2352, Array(raw[0..<12]) == sync, raw[15] == 1 || raw[15] == 2 {
            (stride, offset) = (2352, raw[15] == 1 ? 16 : 24)
        } else {
            (stride, offset) = (2048, 0)
        }
        let pvd = try sector(16)
        guard pvd[0] == 1, Array(pvd[1..<6]) == Array("CD001".utf8) else {
            throw DiscError("No CD file system found in the image")
        }
        root = (le32(pvd, 158), le32(pvd, 166))
    }

    deinit { handle.closeFile() }

    private func le32(_ bytes: [UInt8], _ at: Int) -> UInt32 {
        UInt32(bytes[at]) | UInt32(bytes[at + 1]) << 8 | UInt32(bytes[at + 2]) << 16 | UInt32(bytes[at + 3]) << 24
    }

    func sector(_ n: UInt32) throws -> [UInt8] {
        handle.seek(toFileOffset: UInt64(n) * stride + offset)
        let data = [UInt8](handle.readData(ofLength: 2048))
        guard data.count == 2048 else { throw DiscError("The image ends inside its file system") }
        return data
    }

    private func entries(extent: UInt32, size: UInt32) throws -> [(name: String, directory: Bool, extent: UInt32, size: UInt32)] {
        var result: [(String, Bool, UInt32, UInt32)] = []
        let sectors = (size + 2047) / 2048
        guard sectors < 4096 else { throw DiscError("A disc folder is too large") }
        for index in 0..<sectors {
            let data = try sector(extent + index)
            var i = 0
            while i < 2048 {
                let length = Int(data[i])
                guard length >= 34, i + length <= 2048 else { break }
                let nameLength = Int(data[i + 32])
                if 33 + nameLength <= length {
                    let raw = data[(i + 33)..<(i + 33 + nameLength)]
                    if !(nameLength == 1 && (raw.first == 0 || raw.first == 1)) {
                        var name = String(decoding: raw, as: UTF8.self)
                        if let semicolon = name.firstIndex(of: ";") { name = String(name[..<semicolon]) }
                        while name.hasSuffix(".") { name.removeLast() }
                        result.append((name, data[i + 25] & 2 != 0, le32(data, i + 2), le32(data, i + 10)))
                    }
                }
                i += length
            }
        }
        return result
    }

    /// The extent and size of a file on the disc (for example GEARHEAD/GEAR.MSI), or nil.
    public func find(_ path: String) throws -> (extent: UInt32, size: UInt32)? {
        var current = root
        let parts = path.split(whereSeparator: { $0 == "/" || $0 == "\\" }).map(String.init)
        for (index, part) in parts.enumerated() {
            let wantDirectory = index < parts.count - 1
            guard let match = try entries(extent: current.extent, size: current.size)
                .first(where: { $0.name.lowercased() == part.lowercased() && $0.directory == wantDirectory })
            else { return nil }
            current = (match.extent, match.size)
        }
        return current
    }

    /// SHA-256 of a file on the disc, read sector by sector.
    public func sha256(extent: UInt32, size: UInt32) throws -> String {
        var hasher = SHA256()
        var left = Int(size)
        var n = extent
        while left > 0 {
            let data = try sector(n)
            hasher.update(data: Data(data.prefix(min(2048, left))))
            left -= 2048
            n += 1
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }
}
