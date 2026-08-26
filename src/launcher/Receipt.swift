import Foundation

/// What the last deploy laid down (ADR 0010), as the launcher sees it.
///
/// The app never guesses at its own installation: the version it shows, the
/// compatibility statement it reports and the files `--verify` re-hashes all
/// come from `receipt.json`. A missing receipt is itself an answer — nothing is
/// deployed — and the checklist says so rather than showing a blank.
struct Receipt {
    let version: String
    let deployedAt: String
    let overlay: Bool
    let tested: Compatibility
    let observed: Compatibility
    let fileCount: Int

    struct Compatibility {
        let macOS: String
        let crossover: String
        let steam: String
    }

    static var path: String { ShimPath.inHome(ShimPath.receiptRel) }

    static func load() -> Receipt? {
        guard let data = FileManager.default.contents(atPath: path),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return nil }
        func s(_ key: String) -> String { root[key] as? String ?? "unknown" }
        return Receipt(
            version: s("version"),
            deployedAt: s("deployed_at"),
            overlay: s("overlay") != "0",
            tested: Compatibility(macOS: s("tested_macos"),
                                  crossover: s("tested_crossover"),
                                  steam: s("tested_steam")),
            observed: Compatibility(macOS: s("observed_macos"),
                                    crossover: s("observed_crossover"),
                                    steam: s("observed_steam")),
            fileCount: (root["files"] as? [Any])?.count ?? 0)
    }

    /// The payload carries the script that deployed it, so verifying, rolling
    /// back and uninstalling are the deploy module's job in every caller —
    /// including this one. Reimplementing sha256-over-the-receipt in Swift
    /// would be a second implementation of a check that must not disagree.
    static var deployScript: String { ShimPath.inHome(ShimPath.liveRel) + "/deploy.sh" }
}
