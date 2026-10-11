// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "SentinelBar",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(name: "SentinelBar", path: "Sources/SentinelBar"),
    ]
)
