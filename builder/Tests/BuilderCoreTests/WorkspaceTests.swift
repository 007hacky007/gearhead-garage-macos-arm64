@testable import BuilderCore
import XCTest

final class WorkspaceTests: XCTestCase {
    var folder: URL!

    override func setUp() { folder = temporaryFolder() }
    override func tearDown() { try? FileManager.default.removeItem(at: folder) }

    func testPrepareWorkspaceCopiesAndKeeps() throws {
        let sources = folder.appendingPathComponent("sources")
        let fm = FileManager.default
        try fm.createDirectory(at: sources.appendingPathComponent("tools/__pycache__"), withIntermediateDirectories: true)
        try "x".write(to: sources.appendingPathComponent("tools/builder_cli.py"), atomically: true, encoding: .utf8)
        try "x".write(to: sources.appendingPathComponent("tools/__pycache__/a.pyc"), atomically: true, encoding: .utf8)
        try "capstone".write(to: sources.appendingPathComponent("requirements.txt"), atomically: true, encoding: .utf8)
        try "not bundled".write(to: sources.appendingPathComponent("secret.txt"), atomically: true, encoding: .utf8)
        let paths = BuilderPaths(support: folder.appendingPathComponent("support"))
        let workspace = paths.workspace(version: "1.0")
        try prepareWorkspace(sources: sources, destination: workspace, version: "1.0+a")
        XCTAssertTrue(fm.fileExists(atPath: workspace.appendingPathComponent("tools/builder_cli.py").path))
        XCTAssertFalse(fm.fileExists(atPath: workspace.appendingPathComponent("tools/__pycache__").path))
        XCTAssertFalse(fm.fileExists(atPath: workspace.appendingPathComponent("secret.txt").path))

        // The same marker keeps the workspace (and its build cache) as it is.
        try "cache".write(to: workspace.appendingPathComponent("cache"), atomically: true, encoding: .utf8)
        try prepareWorkspace(sources: sources, destination: workspace, version: "1.0+a")
        XCTAssertTrue(fm.fileExists(atPath: workspace.appendingPathComponent("cache").path))
        // New sources replace it.
        try prepareWorkspace(sources: sources, destination: workspace, version: "1.0+b")
        XCTAssertFalse(fm.fileExists(atPath: workspace.appendingPathComponent("cache").path))

        XCTAssertTrue(buildToolsCheck(paths: paths, version: "1.0").isBlocked)
    }

    func testOldWorkspacesRemoved() throws {
        let paths = BuilderPaths(support: folder)
        let fm = FileManager.default
        for (name, marker) in [("0.9", "0.9+x"), ("1.0", "1.0+y")] {
            try fm.createDirectory(at: folder.appendingPathComponent(name), withIntermediateDirectories: true)
            try marker.write(to: folder.appendingPathComponent("\(name)/.builder-version"), atomically: true, encoding: .utf8)
        }
        try fm.createDirectory(at: paths.python, withIntermediateDirectories: true)
        try fm.createDirectory(at: folder.appendingPathComponent("unmarked"), withIntermediateDirectories: true)
        removeOldWorkspaces(paths, keeping: "1.0")
        XCTAssertEqual(Set(try fm.contentsOfDirectory(atPath: folder.path)), ["1.0", "python", "unmarked"])
    }
}
