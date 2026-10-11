// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "SentinelBar",
    platforms: [.macOS(.v13)],
    targets: [
        .target(name: "SentinelBarCore"),
        .executableTarget(name: "SentinelBar", dependencies: ["SentinelBarCore"], path: "Sources/SentinelBar", exclude: ["Resources"]),
        .testTarget(name: "SentinelBarCoreTests", dependencies: ["SentinelBarCore"]),
    ]
)
