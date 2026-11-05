@testable import BuilderCore
import XCTest

final class CoreTests: XCTestCase {
    func testEvents() {
        XCTAssertEqual(parsePipelineLine("GEARHEAD-EVENT {\"step\": \"lift\", \"title\": \"T\"}"), .step(id: "lift", title: "T"))
        XCTAssertEqual(parsePipelineLine("GEARHEAD-EVENT {\"progress\": 0.5}"), .progress(0.5))
        XCTAssertEqual(parsePipelineLine("GEARHEAD-EVENT {\"progress\": 1}"), .progress(1))
        XCTAssertEqual(parsePipelineLine("GEARHEAD-EVENT {\"done\": \"/x/G.app\"}"), .done(app: URL(fileURLWithPath: "/x/G.app")))
        XCTAssertEqual(parsePipelineLine("GEARHEAD-EVENT {\"error\": \"no\"}"), .error("no"))
        XCTAssertEqual(parsePipelineLine("plain"), .log("plain"))
        XCTAssertEqual(parsePipelineLine("GEARHEAD-EVENT {broken"), .log("GEARHEAD-EVENT {broken"))
    }

    func testProgress() {
        XCTAssertEqual(buildSteps.reduce(0) { $0 + $1.weight }, 100)
        XCTAssertEqual(progressFraction(atStart: "prepare"), 0)
        XCTAssertEqual(progressFraction(in: "package", part: 1), 1, accuracy: 1e-9)
        XCTAssertLessThan(progressFraction(in: "lift", part: 0.5), progressFraction(atStart: "compile"))
    }

    func testCanBuild() {
        var states = Dictionary(uniqueKeysWithValues: CheckID.allCases.map { ($0, CheckState.ready("")) })
        XCTAssertTrue(canBuild(states))
        states[.destination] = .automatic("replaces")
        XCTAssertTrue(canBuild(states))
        states[.gameCopy] = .checking("")
        XCTAssertFalse(canBuild(states))
        states[.gameCopy] = .blocked("")
        XCTAssertFalse(canBuild(states))
    }

    func testMacAndSpace() {
        XCTAssertTrue(macCheck(isAppleSilicon: false, version: OperatingSystemVersion(majorVersion: 14, minorVersion: 0, patchVersion: 0)).isBlocked)
        XCTAssertTrue(macCheck(isAppleSilicon: true, version: OperatingSystemVersion(majorVersion: 10, minorVersion: 15, patchVersion: 0)).isBlocked)
        XCTAssertTrue(macCheck(isAppleSilicon: true, version: OperatingSystemVersion(majorVersion: 11, minorVersion: 0, patchVersion: 0)).isReady)
        XCTAssertTrue(diskSpaceCheck(available: requiredBuildBytes - 1).isBlocked)
        XCTAssertTrue(diskSpaceCheck(available: requiredBuildBytes).isReady)
        XCTAssertEqual(spaceNeeds(workspace: 1, destination: 1), [1: requiredBuildBytes])
        XCTAssertEqual(spaceNeeds(workspace: 1, destination: 2), [1: requiredBuildBytes, 2: appBytes])
    }

    func testCommandLineTools() {
        struct Runner: CommandRunning {
            let output: String
            func run(_ executable: String, _ arguments: [String]) -> (status: Int32, output: String) { (0, output) }
        }
        let present: Set<String> = ["/dev/usr/bin/clang", "/usr/bin/codesign", "/usr/bin/iconutil"]
        XCTAssertTrue(CommandLineTools(runner: Runner(output: "/dev\n"), fileExists: { present.contains($0) }).state().isReady)
        XCTAssertTrue(CommandLineTools(runner: Runner(output: "/other\n"), fileExists: { present.contains($0) }).state().isBlocked)
    }
}
