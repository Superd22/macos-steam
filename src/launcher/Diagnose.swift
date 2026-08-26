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
            return Finding(id: "integrity", title: "Payload integrity", verdict: .blocked,
                           detail: "Nothing deployed: no payload at \(ShimPath.inHome(ShimPath.liveRel)).")
        }
        return Finding(id: "integrity",
                       title: result.ok ? "Payload matches its receipt" : "Payload does not match its receipt",
                       verdict: result.ok ? .ok : .blocked,
                       detail: result.output.trimmingCharacters(in: .whitespacesAndNewlines))
    }

    /// `ps aux | grep -i steam.exe` must be empty. If a Windows Steam is running
    /// in some bottle, it may be what answered a title's Steamworks call, and
    /// the result proves nothing either way.
    static func strayWindowsSteam() -> Finding {
        let running = Shell.isRunning("steam.exe")
        return Finding(id: "stray",
                       title: running ? "A Windows steam.exe is running" : "No Windows Steam running",
                       verdict: running ? .blocked : .ok,
                       detail: running
                         ? "A Windows Steam in some bottle may be what titles are talking to, which makes any result meaningless. Quit it, and remove it from the bottle."
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
            return Finding(id: "plain", title: "Steam is running with the gate open",
                           verdict: .ok, detail: "")
        }
        return Finding(id: "plain", title: "Steam is running, but not through this launcher",
                       verdict: .blocked,
                       detail: "The compat gate is flipped in memory at each launch, so a Steam started from Valve's own icon has it shut and no Windows title will install. Quit Steam and start it from here.")
    }

    /// Loaded vs armed (#30, CONTEXT.md). Conflating them is the failure this
    /// row exists to prevent: a renderer can be in the process with its hooks
    /// installed and still never have completed the client handshake, and a
    /// title told "armed" when it is only "loaded" waits for a panel forever.
    static func overlayState() -> Finding {
        let on = Prefs.overlay
        guard on else {
            return Finding(id: "overlay", title: "Overlay off", verdict: .ok,
                           detail: "Turn it on in Settings; it takes effect at the next Steam launch.")
        }
        let unix = LogWatch.tail(ShimPath.inHome(ShimPath.logUnixRel), bytes: 65536)
        let loaded = unix.contains(ShimPath.overlayDylib)
        if unix.isEmpty {
            return Finding(id: "overlay", title: "Overlay on, never exercised", verdict: .pending,
                           detail: "No title has launched through the shim yet, so there is nothing to report.")
        }
        return Finding(id: "overlay",
                       title: loaded ? "Overlay renderer loaded" : "Overlay on, renderer not seen",
                       verdict: loaded ? .ok : .warning,
                       detail: loaded
                         ? "Loaded means the renderer is in the game process. Armed — the client handshake completed, so a panel can actually appear — is a further step, and only the title can observe it."
                         : "The last run of a title shows no renderer load. The overlay is delivered per-launch; try launching a title again.")
    }

    /// What this build was measured against, beside what this Mac is. Both come
    /// from the receipt, so the statement cannot drift from the release.
    static func compatibility() -> Finding {
        guard let r = Receipt.load() else {
            return Finding(id: "compat", title: "No receipt", verdict: .blocked, detail: "")
        }
        let drift = [r.tested.macOS != r.observed.macOS ? "macOS \(r.observed.macOS) vs \(r.tested.macOS) tested" : nil,
                     r.tested.crossover != r.observed.crossover ? "CrossOver \(r.observed.crossover) vs \(r.tested.crossover) tested" : nil]
            .compactMap { $0 }
        return Finding(id: "compat",
                       title: drift.isEmpty ? "Exercised against exactly this setup" : "Untested combination",
                       verdict: drift.isEmpty ? .ok : .warning,
                       detail: drift.isEmpty
                         ? "\(r.version), tested on macOS \(r.tested.macOS), CrossOver \(r.tested.crossover), \(r.tested.steam)."
                         : "This release was exercised on a different setup: \(drift.joined(separator: "; ")). It may well work; nobody has measured it.")
    }

    /// Everything above, as text a user can paste into an issue. The logs
    /// themselves are deliberately NOT included: they name the whole Steam
    /// library, which is why they live owner-only outside /tmp to begin with.
    static func report() -> String {
        var out = ["macos-steam-shim diagnose"]
        if let r = Receipt.load() {
            out.append("version \(r.version), deployed \(r.deployedAt), overlay \(r.overlay ? "on" : "off")")
            out.append("observed: macOS \(r.observed.macOS), CrossOver \(r.observed.crossover), steam_osx \(r.observed.steam)")
            out.append("tested:   macOS \(r.tested.macOS), CrossOver \(r.tested.crossover), \(r.tested.steam)")
        } else {
            out.append("no receipt — nothing deployed")
        }
        for f in run() {
            out.append("[\(f.verdict)] \(f.title)")
            if !f.detail.isEmpty { out.append("    \(f.detail)") }
        }
        return out.joined(separator: "\n")
    }
}
