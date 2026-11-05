import Foundation

/// The supported release, from tools/game_manifest.json in the bundled sources.
public struct GameManifest: Equatable {
    public let release: String
    public let msiSHA256: String
    public let exeSHA256: String
    public let exeSize: Int
    public let fileCount: Int
    public let totalBytes: Int

    public init(release: String, msiSHA256: String, exeSHA256: String, exeSize: Int, fileCount: Int, totalBytes: Int) {
        (self.release, self.msiSHA256, self.exeSHA256) = (release, msiSHA256, exeSHA256)
        (self.exeSize, self.fileCount, self.totalBytes) = (exeSize, fileCount, totalBytes)
    }

    public init(contentsOf url: URL) throws {
        let data = try Data(contentsOf: url)
        guard let object = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              let release = object["release"] as? String, let msi = object["msi_sha256"] as? String,
              let files = object["files"] as? [[String: Any]],
              let exe = files.first(where: { ($0["path"] as? String)?.lowercased() == "ghg.exe" }),
              let exeSHA = exe["sha256"] as? String, let exeSize = exe["size"] as? Int
        else { throw DiscError("The bundled game manifest is damaged") }
        let total = files.reduce(0) { $0 + (($1["size"] as? Int) ?? 0) }
        self.init(release: release, msiSHA256: msi, exeSHA256: exeSHA, exeSize: exeSize, fileCount: files.count, totalBytes: total)
    }
}

/// What the player chose as the game copy.
public enum GameCopyKind: Equatable {
    /// An installed game folder (holds ghg.exe), possibly the game inside a Gearhead Garage.app.
    case installed(folder: URL, insideApp: Bool)
    /// GEAR.MSI itself, or a disc folder that holds GEARHEAD/GEAR.MSI.
    case package(msi: URL)
    /// A CD image: .cue (with its .bin), .bin, .img or .iso.
    case image(URL)
}

public struct GameCopyReport: Equatable {
    public let state: CheckState
    /// The checksum that did not match, for Copy details.
    public let details: String?
}

/// The path of `relative` (a/b/c) under `base`, each part matched without case.
public func findInsensitive(_ base: URL, _ relative: String) -> URL? {
    var current = base
    let fm = FileManager.default
    for part in relative.split(separator: "/") {
        guard let names = try? fm.contentsOfDirectory(atPath: current.path),
              let match = names.first(where: { $0.lowercased() == part.lowercased() })
        else { return nil }
        current = current.appendingPathComponent(match)
    }
    return current
}

public let imageExtensions: Set<String> = ["cue", "bin", "img", "iso"]

public func classifyGameCopy(_ url: URL) -> Result<GameCopyKind, DiscError> {
    let fm = FileManager.default
    var isDirectory: ObjCBool = false
    guard fm.fileExists(atPath: url.path, isDirectory: &isDirectory) else {
        return .failure(DiscError("\(url.lastPathComponent) is no longer there"))
    }
    let ext = url.pathExtension.lowercased()
    if isDirectory.boolValue {
        if ext == "app" {
            let inner = url.appendingPathComponent("Contents/Resources/Game")
            guard findInsensitive(inner, "ghg.exe") != nil else {
                return .failure(DiscError("\(url.lastPathComponent) has no Gearhead Garage game inside it"))
            }
            return .success(.installed(folder: inner, insideApp: true))
        }
        if findInsensitive(url, "ghg.exe") != nil { return .success(.installed(folder: url, insideApp: false)) }
        if let msi = findInsensitive(url, "GEARHEAD/GEAR.MSI") ?? findInsensitive(url, "GEAR.MSI") {
            return .success(.package(msi: msi))
        }
        return .failure(DiscError("This folder has neither ghg.exe (an installed game) nor GEARHEAD/GEAR.MSI (the disc)"))
    }
    if ext == "msi" { return .success(.package(msi: url)) }
    if imageExtensions.contains(ext) { return .success(.image(url)) }
    return .failure(DiscError("Choose a CD image (.cue, .bin, .iso), GEAR.MSI, the disc or an installed game folder"))
}

/// Checks the game copy by its one file that decides the release: ghg.exe in an installed
/// folder, GEAR.MSI on the disc. The build checks all of the game's files again.
public func checkGameCopy(_ url: URL, manifest: GameManifest) -> GameCopyReport {
    let kind: GameCopyKind
    switch classifyGameCopy(url) {
    case let .failure(error): return GameCopyReport(state: .blocked(error.message), details: nil)
    case let .success(found): kind = found
    }
    func verdict(_ found: String?, expected: String, file: String, ready: String) -> GameCopyReport {
        guard let found = found else {
            return GameCopyReport(state: .blocked("\(file) cannot be read"), details: nil)
        }
        if found == expected { return GameCopyReport(state: .ready(ready), details: nil) }
        return GameCopyReport(
            state: .blocked("This is not the supported release: its \(file) is a different version. Click \"Which copy works?\""),
            details: "\(file) SHA-256 \(found)\nexpected \(expected) (\(manifest.release))")
    }
    switch kind {
    case let .installed(folder, insideApp):
        let exe = findInsensitive(folder, "ghg.exe")!
        let what = insideApp ? "The game inside \(url.lastPathComponent)" : "Installed game"
        return verdict(try? sha256Hex(of: exe), expected: manifest.exeSHA256, file: "ghg.exe",
                       ready: "\(what), Build 131 (ghg.exe checked)")
    case let .package(msi):
        return verdict(try? sha256Hex(of: msi), expected: manifest.msiSHA256, file: "GEAR.MSI",
                       ready: "Build 131 installer, GEAR.MSI checked")
    case let .image(image):
        do {
            let disc = try Disc(image)
            guard let file = try disc.find("GEARHEAD/GEAR.MSI") else {
                return GameCopyReport(state: .blocked("This CD has no GEARHEAD/GEAR.MSI, so it is not the supported release"), details: nil)
            }
            return verdict(try disc.sha256(extent: file.extent, size: file.size), expected: manifest.msiSHA256,
                           file: "GEAR.MSI", ready: "CD image, Build 131 (GEAR.MSI checked)")
        } catch let error as DiscError {
            return GameCopyReport(state: .blocked(error.message), details: nil)
        } catch {
            return GameCopyReport(state: .blocked(error.localizedDescription), details: nil)
        }
    }
}
