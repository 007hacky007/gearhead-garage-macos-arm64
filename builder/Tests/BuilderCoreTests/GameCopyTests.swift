@testable import BuilderCore
import XCTest

final class GameCopyTests: XCTestCase {
    let installer = Data((0..<5000).map { UInt8($0 % 251) })
    let exe = Data("MZ test program".utf8)
    var folder: URL!
    var manifest: GameManifest!

    override func setUp() {
        folder = temporaryFolder()
        manifest = GameManifest(release: "Test release", msiSHA256: sha(installer), exeSHA256: sha(exe), exeSize: exe.count,
                                fileCount: 2, totalBytes: 10)
    }

    override func tearDown() { try? FileManager.default.removeItem(at: folder) }

    func testDiscModes() throws {
        for mode: UInt8? in [nil, 1, 2] {
            let image = folder.appendingPathComponent("disc-\(mode ?? 0).bin")
            try makeImage(["GEARHEAD/GEAR.MSI": installer, "OTHER/A.TXT": Data("a".utf8)], raw: mode).write(to: image)
            let disc = try Disc(image)
            let file = try XCTUnwrap(try disc.find("gearhead/gear.msi"))
            XCTAssertEqual(file.size, UInt32(installer.count))
            XCTAssertEqual(try disc.sha256(extent: file.extent, size: file.size), sha(installer))
            XCTAssertNil(try disc.find("GEARHEAD/NONE.MSI"))
            XCTAssertNil(try disc.find("GEARHEAD"))
        }
    }

    func testImageChecks() throws {
        let bin = folder.appendingPathComponent("disc.bin")
        try makeImage(["GEARHEAD/GEAR.MSI": installer], raw: 2).write(to: bin)
        let cue = folder.appendingPathComponent("disc.cue")
        try "FILE \"disc.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n".write(to: cue, atomically: true, encoding: .utf8)
        XCTAssertTrue(checkGameCopy(cue, manifest: manifest).state.isReady)
        XCTAssertTrue(checkGameCopy(bin, manifest: manifest).state.isReady)

        let other = folder.appendingPathComponent("other.iso")
        try makeImage(["GEARHEAD/GEAR.MSI": Data("different".utf8)]).write(to: other)
        let report = checkGameCopy(other, manifest: manifest)
        XCTAssertTrue(report.state.isBlocked)
        XCTAssertNotNil(report.details)

        let empty = folder.appendingPathComponent("empty.iso")
        try makeImage(["OTHER/A.TXT": Data("a".utf8)]).write(to: empty)
        XCTAssertTrue(checkGameCopy(empty, manifest: manifest).state.isBlocked)

        let junk = folder.appendingPathComponent("junk.iso")
        try Data(count: 50000).write(to: junk)
        XCTAssertTrue(checkGameCopy(junk, manifest: manifest).state.isBlocked)
    }

    func testCueRefusals() throws {
        let cue = folder.appendingPathComponent("bad.cue")
        try "FILE \"../x.bin\" BINARY\n  TRACK 01 MODE1/2352\n".write(to: cue, atomically: true, encoding: .utf8)
        XCTAssertThrowsError(try cueImage(cue))
        try "FILE \"x.bin\" BINARY\n  TRACK 01 AUDIO\n".write(to: cue, atomically: true, encoding: .utf8)
        XCTAssertThrowsError(try cueImage(cue))
    }

    func testFoldersAndApps() throws {
        let fm = FileManager.default
        let installed = folder.appendingPathComponent("Installed")
        try fm.createDirectory(at: installed, withIntermediateDirectories: true)
        try exe.write(to: installed.appendingPathComponent("GHG.EXE"))
        XCTAssertEqual(try classifyGameCopy(installed).get(), .installed(folder: installed, insideApp: false))
        XCTAssertTrue(checkGameCopy(installed, manifest: manifest).state.isReady)

        try Data("other".utf8).write(to: installed.appendingPathComponent("GHG.EXE"))
        XCTAssertTrue(checkGameCopy(installed, manifest: manifest).state.isBlocked)

        let disc = folder.appendingPathComponent("Disc/gearhead")
        try fm.createDirectory(at: disc, withIntermediateDirectories: true)
        try installer.write(to: disc.appendingPathComponent("gear.msi"))
        XCTAssertTrue(checkGameCopy(folder.appendingPathComponent("Disc"), manifest: manifest).state.isReady)
        XCTAssertTrue(checkGameCopy(disc.appendingPathComponent("gear.msi"), manifest: manifest).state.isReady)

        let app = folder.appendingPathComponent("Gearhead Garage.app")
        try fm.createDirectory(at: app.appendingPathComponent("Contents/Resources/Game"), withIntermediateDirectories: true)
        XCTAssertTrue(checkGameCopy(app, manifest: manifest).state.isBlocked)
        try exe.write(to: app.appendingPathComponent("Contents/Resources/Game/ghg.exe"))
        XCTAssertTrue(checkGameCopy(app, manifest: manifest).state.isReady)

        let nothing = folder.appendingPathComponent("Nothing")
        try fm.createDirectory(at: nothing, withIntermediateDirectories: true)
        XCTAssertTrue(checkGameCopy(nothing, manifest: manifest).state.isBlocked)
        XCTAssertTrue(checkGameCopy(folder.appendingPathComponent("missing.iso"), manifest: manifest).state.isBlocked)
    }

    func testBundledManifest() throws {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        let manifest = try GameManifest(contentsOf: root.appendingPathComponent("tools/game_manifest.json"))
        XCTAssertEqual(manifest.fileCount, 122)
        XCTAssertEqual(manifest.exeSHA256, "f9d797d580d5a42f2f46b912cd6c107bad34c27a32d050cbee13379a65cc498c")
        XCTAssertEqual(manifest.msiSHA256, "8df931e84afeb470f9bca15b9d2926bd5102591c919462b94a8c291c66617f6e")
    }
}
