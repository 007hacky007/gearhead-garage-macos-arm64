import Foundation

public let appName = "Gearhead Garage.app"
public let appBundleID = "org.classicgames.gearheadgarage"

public let defaultDestination = URL(fileURLWithPath: "/Applications", isDirectory: true)

/// A Gearhead Garage.app made by this build (or the Terminal build) in a folder.
public struct InstalledApp: Equatable {
    public let url: URL
    public let version: String?
    public let builderVersion: String?
    /// The game inside the app, when there is one.
    public let hasGame: Bool

    public static func find(in folder: URL) -> InstalledApp? {
        let app = folder.appendingPathComponent(appName)
        guard let data = try? Data(contentsOf: app.appendingPathComponent("Contents/Info.plist")),
              let info = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
              info["CFBundleIdentifier"] as? String == appBundleID
        else { return nil }
        let game = findInsensitive(app.appendingPathComponent("Contents/Resources/Game"), "ghg.exe") != nil
        return InstalledApp(url: app, version: info["CFBundleShortVersionString"] as? String,
                            builderVersion: info["GearheadBuilderVersion"] as? String, hasGame: game)
    }
}

/// Something named Gearhead Garage.app that this build did not make: never replaced.
public func foreignAppExists(in folder: URL) -> Bool {
    let app = folder.appendingPathComponent(appName)
    return FileManager.default.fileExists(atPath: app.path) && InstalledApp.find(in: folder) == nil
}

public func destinationCheck(_ folder: URL) -> CheckState {
    let fm = FileManager.default
    var isDirectory: ObjCBool = false
    guard fm.fileExists(atPath: folder.path, isDirectory: &isDirectory), isDirectory.boolValue else {
        return .blocked("\(folder.path) is not there")
    }
    guard fm.isWritableFile(atPath: folder.path) else { return .blocked("You cannot write to \(folder.path)") }
    if foreignAppExists(in: folder) {
        return .blocked("\(folder.path) already has another \(appName); choose another folder")
    }
    if let existing = InstalledApp.find(in: folder) {
        return .automatic("Replaces the \(appName) there (\(existing.version ?? "older")); saves stay")
    }
    return .ready(folder.path)
}

/// "Build Gearhead Garage.app" or "Update Gearhead Garage.app".
public func buildButtonTitle(replacing existing: InstalledApp?) -> String {
    existing == nil ? "Build \(appName)" : "Update \(appName)"
}

/// Puts the built app at `folder`/Gearhead Garage.app. One made by this build that is there
/// already is replaced only once the new one is in place; anything else is never touched.
public func installApp(_ built: URL, into folder: URL) throws -> URL {
    let fm = FileManager.default
    let target = folder.appendingPathComponent(appName)
    if foreignAppExists(in: folder) {
        throw DiscError("\(folder.path) already has another \(appName)")
    }
    let staged = folder.appendingPathComponent(".\(appName).tmp-\(UUID().uuidString)")
    try fm.copyItem(at: built, to: staged)
    do {
        if fm.fileExists(atPath: target.path) {
            let old = try moveToTrash(target)
            try fm.moveItem(at: staged, to: target)
            try? fm.removeItem(at: old)
        } else {
            try fm.moveItem(at: staged, to: target)
        }
    } catch {
        try? fm.removeItem(at: staged)
        throw error
    }
    try? fm.removeItem(at: built)
    return target
}
