import Foundation

public enum PlanLocation {
    public static func researchDirectory(repository: String, configured: String = "") -> URL {
        let repo = URL(fileURLWithPath: (repository as NSString).expandingTildeInPath)
        let configuredPath = (configured.trimmingCharacters(in: .whitespacesAndNewlines) as NSString).expandingTildeInPath
        if configuredPath.isEmpty {
            return repo.deletingLastPathComponent().appendingPathComponent("Sentinel-Private/research")
                .standardizedFileURL
        }
        return (configuredPath.hasPrefix("/") ? URL(fileURLWithPath: configuredPath) : repo.appendingPathComponent(configuredPath))
            .standardizedFileURL
    }

    public static func resolve(_ reference: String, directory: URL, pinned: Bool = false) -> URL {
        let expanded = (reference as NSString).expandingTildeInPath
        if pinned && expanded.hasPrefix("/") {
            return URL(fileURLWithPath: expanded).standardizedFileURL
        }
        let components = expanded.components(separatedBy: "/")
        let relative: String
        if let research = components.lastIndex(of: "research"), research + 1 < components.count {
            // Git trailers identify the plan; its contents come from the private directory.
            relative = components.dropFirst(research + 1).joined(separator: "/")
        } else {
            relative = pinned ? expanded : URL(fileURLWithPath: expanded).lastPathComponent
        }
        return directory.appendingPathComponent(relative).standardizedFileURL
    }
}
