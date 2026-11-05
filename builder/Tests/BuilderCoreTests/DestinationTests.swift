@testable import BuilderCore
import XCTest

final class DestinationTests: XCTestCase {
    var folder: URL!

    override func setUp() { folder = temporaryFolder() }
    override func tearDown() { try? FileManager.default.removeItem(at: folder) }

    private func makeApp(at url: URL, bundleID: String = appBundleID, marker: String) throws {
        let contents = url.appendingPathComponent("Contents")
        try FileManager.default.createDirectory(at: contents, withIntermediateDirectories: true)
        let info: [String: Any] = ["CFBundleIdentifier": bundleID, "CFBundleShortVersionString": "0.1.0"]
        try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0)
            .write(to: contents.appendingPathComponent("Info.plist"))
        try marker.write(to: contents.appendingPathComponent("marker"), atomically: true, encoding: .utf8)
    }

    func testInstallAndReplace() throws {
        let built = folder.appendingPathComponent("work/\(appName)")
        let destination = folder.appendingPathComponent("dest")
        try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true)
        XCTAssertTrue(destinationCheck(destination).isReady)

        try makeApp(at: built, marker: "one")
        let target = try installApp(built, into: destination)
        XCTAssertEqual(try String(contentsOf: target.appendingPathComponent("Contents/marker"), encoding: .utf8), "one")
        XCTAssertFalse(FileManager.default.fileExists(atPath: built.path))
        XCTAssertEqual(InstalledApp.find(in: destination)?.version, "0.1.0")
        XCTAssertEqual(destinationCheck(destination), .automatic("Replaces the \(appName) there (0.1.0); saves stay"))

        try makeApp(at: built, marker: "two")
        _ = try installApp(built, into: destination)
        XCTAssertEqual(try String(contentsOf: target.appendingPathComponent("Contents/marker"), encoding: .utf8), "two")
        let leftovers = try FileManager.default.contentsOfDirectory(atPath: destination.path)
        XCTAssertEqual(leftovers, [appName])
    }

    func testForeignAppIsNeverReplaced() throws {
        let destination = folder.appendingPathComponent("dest")
        try makeApp(at: destination.appendingPathComponent(appName), bundleID: "com.example.other", marker: "theirs")
        XCTAssertTrue(destinationCheck(destination).isBlocked)
        let built = folder.appendingPathComponent("work/\(appName)")
        try makeApp(at: built, marker: "ours")
        XCTAssertThrowsError(try installApp(built, into: destination))
        XCTAssertEqual(try String(contentsOf: destination.appendingPathComponent("\(appName)/Contents/marker"), encoding: .utf8), "theirs")
    }

    func testButtonTitle() throws {
        XCTAssertEqual(buildButtonTitle(replacing: nil), "Build Gearhead Garage.app")
        let app = InstalledApp(url: folder, version: "0.1.0", builderVersion: nil, hasGame: true)
        XCTAssertEqual(buildButtonTitle(replacing: app), "Update Gearhead Garage.app")
    }
}
