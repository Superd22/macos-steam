import Foundation

/// Reading the injector's log so the user never has to (#42).
///
/// Until now, "did it work?" was answered by the user opening
/// `compat-enabler.log` and looking for `patched 1 site(s)`. That line is a
/// precise, trustworthy signal — it is just addressed to the wrong reader. The
/// app watches for it and says "ready".
enum LogWatch {
    /// The marker the injector writes once it has flipped the gate. `patched 0
    /// site(s)` is the interesting failure: the injector ran, found no arm64
    /// gate pattern, and the client is up with Steam Play still off — which is
    /// what an x86_64-translated launch looks like (ADR 0002's correction).
    static let marker = "patched "

    static var enablerLog: String { ShimPath.inHome(ShimPath.logEnablerRel) }

    enum Outcome: Equatable {
        case patched(Int)
        case timedOut
    }

    static func size(of path: String) -> UInt64 {
        ((try? FileManager.default.attributesOfItem(atPath: path))?[.size] as? UInt64) ?? 0
    }

    /// Poll from a known offset rather than reading the whole file: the log
    /// accumulates across launches, and a previous session's success must not
    /// be reported as this one's.
    static func awaitPatch(after offset: UInt64,
                           timeout: TimeInterval = 90,
                           poll: TimeInterval = 0.4,
                           onProgress: @escaping (Outcome?) -> Void) {
        DispatchQueue.global(qos: .utility).async {
            let deadline = Date().addingTimeInterval(timeout)
            while Date() < deadline {
                if let sites = patchedSites(in: read(enablerLog, from: offset)) {
                    DispatchQueue.main.async { onProgress(.patched(sites)) }
                    return
                }
                Thread.sleep(forTimeInterval: poll)
            }
            DispatchQueue.main.async { onProgress(.timedOut) }
        }
    }

    static func patchedSites(in text: String) -> Int? {
        guard let range = text.range(of: marker, options: .backwards) else { return nil }
        let rest = text[range.upperBound...]
        let digits = rest.prefix { $0.isNumber }
        return Int(digits)
    }

    /// Was the gate patched during the run of Steam that is up right now? The
    /// injector writes its line within milliseconds of `steam_osx` starting, so
    /// "the log was written after the process started" is the same question and
    /// needs no cooperation from the running client.
    static func gatePatchedSinceSteamStarted() -> Bool {
        let started = Shell.run("/bin/ps", ["-o", "lstart=", "-p", steamPid() ?? "0"])
        guard started.ok, !started.output.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
              let logDate = try? FileManager.default.attributesOfItem(atPath: enablerLog)[.modificationDate] as? Date
        else { return false }
        let fmt = DateFormatter()
        fmt.dateFormat = "EEE MMM d HH:mm:ss yyyy"
        fmt.locale = Locale(identifier: "en_US_POSIX")
        guard let procDate = fmt.date(from: started.output.trimmingCharacters(in: .whitespacesAndNewlines))
        else { return false }
        // A second of slack: the log line and the process start are the same
        // instant to any resolution `ps` reports.
        return logDate.timeIntervalSince(procDate) > -1
    }

    private static func steamPid() -> String? {
        let out = Shell.run("/usr/bin/pgrep", ["-x", ShimPath.steamOsx]).output
        return out.split(separator: "\n").first.map(String.init)
    }

    static func read(_ path: String, from offset: UInt64) -> String {
        guard let handle = FileHandle(forReadingAtPath: path) else { return "" }
        defer { try? handle.close() }
        try? handle.seek(toOffset: offset)
        let data = (try? handle.readToEnd()) ?? Data()
        return String(data: data, encoding: .utf8) ?? ""
    }

    /// The last N bytes of a log — enough to answer "what happened on the most
    /// recent run" without loading a session's worth of seam traffic.
    static func tail(_ path: String, bytes: UInt64) -> String {
        let total = size(of: path)
        return read(path, from: total > bytes ? total - bytes : 0)
    }
}
