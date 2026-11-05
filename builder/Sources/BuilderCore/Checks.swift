import Foundation

public enum CheckID: CaseIterable {
    case mac, commandLineTools, buildTools, gameCopy, destination, diskSpace
}

/// Grey (checking), green (ready), amber (handled or informational, not blocking), red (blocked).
public enum CheckState: Equatable {
    case checking(String)
    case ready(String)
    case automatic(String)
    case blocked(String)

    public var message: String {
        switch self {
        case let .checking(text), let .ready(text), let .automatic(text), let .blocked(text): return text
        }
    }

    public var isReady: Bool {
        if case .ready = self { return true }
        return false
    }

    public var isBlocked: Bool {
        if case .blocked = self { return true }
        return false
    }
}

public func canBuild(_ states: [CheckID: CheckState]) -> Bool {
    CheckID.allCases.allSatisfy { id in
        switch states[id] {
        case .ready?, .automatic?: return true
        default: return false
        }
    }
}

public func macCheck(isAppleSilicon: Bool, version: OperatingSystemVersion) -> CheckState {
    guard isAppleSilicon else {
        return .blocked("Gearhead Garage needs a Mac with Apple Silicon (M1 or later)")
    }
    guard version.majorVersion >= 11 else {
        return .blocked("Gearhead Garage needs macOS 11 or newer")
    }
    return .ready("Apple Silicon, macOS \(version.majorVersion).\(version.minorVersion)")
}

/// True on Apple Silicon, also when this process runs translated by Rosetta.
public func runningOnAppleSilicon() -> Bool {
    var value: Int32 = 0
    var size = MemoryLayout<Int32>.size
    if sysctlbyname("hw.optional.arm64", &value, &size, nil, 0) == 0 { return value == 1 }
    #if arch(arm64)
    return true
    #else
    return false
    #endif
}

/// Free space a build needs in the Builder's folder: the private Python and its packages
/// (about 110 MB), the unpacked game (44 MB), the translated C code (40 MB), the compiled
/// objects (125 MB) and the app (111 MB) before it is moved, with room to spare.
public let requiredBuildBytes: Int64 = 1_000_000_000
/// The finished Gearhead Garage.app with the game inside, with room to spare.
public let appBytes: Int64 = 150_000_000

/// Bytes each volume needs: the build in the workspace volume, and the app on the
/// destination volume when that is another one (on the same volume it is only moved).
public func spaceNeeds(workspace: UInt64, destination: UInt64?) -> [UInt64: Int64] {
    var needs: [UInt64: Int64] = [workspace: requiredBuildBytes]
    if let destination = destination, destination != workspace {
        needs[destination, default: 0] += appBytes
    }
    return needs
}

public func diskSpaceCheck(available: Int64, required: Int64 = requiredBuildBytes, disk: String? = nil) -> CheckState {
    let gb = { (b: Int64) in String(format: "%.1f GB", Double(b) / 1_000_000_000) }
    if available >= required {
        return .ready("\(gb(available)) free")
    }
    let on = disk.map { " on \($0)" } ?? ""
    return .blocked("Only \(gb(available)) free\(on), \(gb(required)) needed")
}
