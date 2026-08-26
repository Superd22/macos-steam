import Foundation

/// The README's troubleshooting table, executed instead of read (#42).
///
/// Every row here was a paragraph a user had to find, translate into a command,
/// and interpret. The gap that closes is not convenience: two of these — an
/// offline client and a stray Windows Steam — are *convincing false negatives*,
/// where everything looks wired and nothing works, and a user who does not know
/// to suspect them concludes the whole thing is broken.
enum Diagnose {
    struct Finding: Identifiable {
        let id: String
        let title: String
        let verdict: Verdict
        let detail: String
    }

    static func run() -> [Finding] {
        [integrity(), strayWindowsSteam(), plainSteam(), overlayState(), compatibility()]
    }

    /// Is what is on disk what was shipped? The deploy module answers, because
    /// it is the one that wrote the receipt (ADR 0010).
    static func integrity() -> Finding {
        let result = Shell.run(Receipt.deployScript, ["--verify"])
        guard FileManager.default.isExecutableFile(atPath: Receipt.deployScript) else {
            return Finding(id: "integrity", title: "Steam Play is not installed", verdict: .blocked,
                           detail: "Reinstall to put the files in place.")
        }
        return Finding(id: "integrity",
                       title: result.ok ? "Files are intact" : "Some files have changed or gone missing",
                       verdict: result.ok ? .ok : .blocked,
                       // The raw output is a list of the files that failed, which
                       // is the one place a path is more use to the reader than a
                       // sentence about it. On success it says nothing.
                       detail: result.ok ? "" : result.output.trimmingCharacters(in: .whitespacesAndNewlines))
    }

    /// `ps aux | grep -i steam.exe` must be empty. If a Windows Steam is running
    /// in some bottle, it may be what answered a title's Steamworks call, and
    /// the result proves nothing either way.
    static func strayWindowsSteam() -> Finding {
        let running = Shell.isRunning("steam.exe")
        return Finding(id: "stray",
                       title: running ? "A Windows copy of Steam is running" : "No Windows Steam running",
                       verdict: running ? .blocked : .ok,
                       detail: running
                         ? "Your games may be talking to that instead of the Steam on your Mac. Quit it, and delete it from the bottle."
                         : "")
    }

    /// "You launched plain Steam.app" as a recognised state, rather than as a
    /// mystery. A client that is running while our injector never ran this
    /// session is the signature: the gate is flipped in memory per launch, so a
    /// Steam started any other way has it shut.
    static func plainSteam() -> Finding {
        guard Shell.isRunning(ShimPath.steamOsx) else {
            return Finding(id: "plain", title: "Steam is not running", verdict: .ok, detail: "")
        }
        if LogWatch.gatePatchedSinceSteamStarted() {
            return Finding(id: "plain", title: "Steam is running with Steam Play on",
                           verdict: .ok, detail: "")
        }
        return Finding(id: "plain", title: "Steam is running, but not from this app",
                       verdict: .blocked,
                       detail: "Steam Play only switches on when Steam starts from here. Quit Steam and open it from this app.")
    }

    /// Loaded vs armed (#30, CONTEXT.md). Conflating them is the failure this
    /// row exists to prevent: a renderer can be in the process with its hooks
    /// installed and still never have completed the client handshake, and a
    /// title told "armed" when it is only "loaded" waits for a panel forever.
    static func overlayState() -> Finding {
        let on = Prefs.overlay
        guard on else {
            return Finding(id: "overlay", title: "Overlay off", verdict: .ok,
                           detail: "Turn it on in Settings. It takes effect the next time Steam starts.")
        }
        let unix = LogWatch.tail(ShimPath.inHome(ShimPath.logUnixRel), bytes: 65536)
        let loaded = unix.contains(ShimPath.overlayDylib)
        if unix.isEmpty {
            return Finding(id: "overlay", title: "Overlay on, not used yet", verdict: .pending,
                           detail: "No Windows game has run yet, so there is nothing to report.")
        }
        return Finding(id: "overlay",
                       title: loaded ? "Overlay reached your last game" : "Overlay on, but it did not reach your last game",
                       verdict: loaded ? .ok : .warning,
                       // Loaded is not armed (CONTEXT.md), and the difference is
                       // real — but it is ours, not the reader's. The row claims
                       // only what was seen: the overlay got there. Whether a
                       // panel opens is Steam's business and a user finds out by
                       // pressing Shift+Tab, not by reading about a handshake.
                       detail: loaded ? "" : "Try starting the game again.")
    }

    /// What this build was measured against, beside what this Mac is. Both come
    /// from the receipt, so the statement cannot drift from the release.
    static func compatibility() -> Finding {
        guard let r = Receipt.load() else {
            return Finding(id: "compat", title: "Steam Play is not installed", verdict: .blocked, detail: "")
        }
        let drift = [r.tested.macOS != r.observed.macOS ? "macOS \(r.observed.macOS) vs \(r.tested.macOS) tested" : nil,
                     r.tested.crossover != r.observed.crossover ? "CrossOver \(r.observed.crossover) vs \(r.tested.crossover) tested" : nil]
            .compactMap { $0 }
        return Finding(id: "compat",
                       title: drift.isEmpty ? "Tested with your setup" : "Your setup has not been tested",
                       verdict: drift.isEmpty ? .ok : .warning,
                       detail: drift.isEmpty
                         ? "Version \(r.version), tested with macOS \(r.tested.macOS) and CrossOver \(r.tested.crossover)."
                         : "Yours differs: \(drift.joined(separator: "; ")). It may work fine, but nobody has checked.")
    }

    /// Everything above, as text a user can paste into an issue. The logs
    /// themselves are deliberately NOT included: they name the whole Steam
    /// library, which is why they live owner-only outside /tmp to begin with.
    static func report() -> String {
        var out = ["macos-steam-shim report"]
        if let r = Receipt.load() {
            out.append("version \(r.version), installed \(r.deployedAt), overlay \(r.overlay ? "on" : "off")")
            out.append("this Mac:    macOS \(r.observed.macOS), CrossOver \(r.observed.crossover), Steam \(r.observed.steam)")
            out.append("tested with: macOS \(r.tested.macOS), CrossOver \(r.tested.crossover), \(r.tested.steam)")
        } else {
            out.append("nothing installed")
        }
        for f in run() {
            out.append("[\(f.verdict)] \(f.title)")
            if !f.detail.isEmpty { out.append("    \(f.detail)") }
        }
        return out.joined(separator: "\n")
    }
}
