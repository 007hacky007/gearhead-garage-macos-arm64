// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "GearheadBuilder",
    platforms: [.macOS(.v11)],
    targets: [
        .target(name: "BuilderCore"),
        .executableTarget(name: "GearheadBuilder", dependencies: ["BuilderCore"]),
        .testTarget(name: "BuilderCoreTests", dependencies: ["BuilderCore"]),
    ]
)
