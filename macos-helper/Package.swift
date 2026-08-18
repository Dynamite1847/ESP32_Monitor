// swift-tools-version: 5.10

import PackageDescription

let package = Package(
    name: "DeskConsoleHelper",
    platforms: [
        .macOS(.v13),
    ],
    products: [
        .executable(
            name: "DeskConsoleHelper",
            targets: ["DeskConsoleHelper"]
        ),
    ],
    targets: [
        .executableTarget(
            name: "DeskConsoleHelper",
            path: "Sources/DeskConsoleHelper"
        ),
    ]
)
