import SwiftUI

/// What the two panes share: the preflight verdicts, the first-run launch that
/// proves itself, and the long-running actions (verify, bottle creation,
/// uninstall) that must not run on the main thread.
@MainActor
final class AppModel: ObservableObject {
    @Published var checks: [Check] = Preflight.run()
    @Published var findings: [Diagnose.Finding] = []
    @Published var busy: String?
    @Published var launchState: LaunchState = .idle
    @Published var overlay: Bool = Prefs.overlay
    @Published var overlayPendingRestart = false

    enum LaunchState: Equatable {
        case idle
        case launching                 // spawned; watching the log
        case ready(sites: Int)         // `patched N site(s)` seen, with N >= 1
        case launchedButUnproven(String)
    }

    var receipt: Receipt? { Receipt.load() }
    var version: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String
            ?? receipt?.version ?? "unknown"
    }

    func refresh() {
        checks = Preflight.run()
    }

    /// The first-run launch, and the only launch this app does not exec into.
    /// Staying alive is the whole point: it is what lets the app read
    /// `patched 1 site(s)` itself and tell the user, rather than sending them
    /// to a log file — check 6 of the preflight list.
    /// True when a client is already up that we did not open the gate for.
    /// Starting Steam again in that state proves nothing: Valve's single-
    /// instance handling forwards the second launch to the running client, so
    /// the injector runs in a process that immediately exits — it writes
    /// `patched 1 site(s)` for a client nobody is using.
    var mustQuitSteamFirst: Bool {
        Shell.isRunning(ShimPath.steamOsx) && !LogWatch.gatePatchedSinceSteamStarted()
    }

    func launchAndProve() {
        if mustQuitSteamFirst {
            busy = "Quitting the Steam that is already running…"
            Task.detached {
                Shell.run("/usr/bin/osascript", ["-e", "quit app \"Steam\""])
                for _ in 0..<40 where Shell.isRunning(ShimPath.steamOsx) {
                    try? await Task.sleep(nanoseconds: 500_000_000)
                }
                await MainActor.run { self.busy = nil; self.spawnAndWatch() }
            }
            return
        }
        spawnAndWatch()
    }

    private func spawnAndWatch() {
        let offset = LogWatch.size(of: LogWatch.enablerLog)
        guard Launch.spawn(overlay: overlay) != nil else {
            launchState = .launchedButUnproven("Steam could not be started at all.")
            return
        }
        launchState = .launching
        LogWatch.awaitPatch(after: offset) { [weak self] outcome in
            guard let self else { return }
            switch outcome {
            case .patched(let sites) where sites >= 1:
                self.launchState = .ready(sites: sites)
                Prefs.firstRunCompleted = true
                self.refresh()
            case .patched:
                // `patched 0 site(s)`: the injector ran and found no gate. The
                // known cause is an x86_64-translated launch, where the arm64
                // pattern matches nothing (ADR 0002's second correction).
                self.launchState = .launchedButUnproven(
                    "Steam started, but the compat gate was not found. This is what a translated x86_64 launch looks like — the pattern is arm64 code.")
            case .timedOut, .none:
                self.launchState = .launchedButUnproven(
                    "Steam started, but nothing reported flipping the compat gate. Open Diagnose for the details.")
            }
        }
    }

    func createBottle() {
        perform("Creating the bottle…") { Preflight.createBottle().output }
    }

    func runDiagnose() {
        busy = "Checking…"
        Task.detached {
            let found = Diagnose.run()
            await MainActor.run {
                self.findings = found
                self.busy = nil
            }
        }
    }

    /// Changing the overlay takes effect at the next Steam launch, because the
    /// environment a compat tool sees is captured when Steam starts. Per
    /// session, not per title — per title is plumbable through the compat tool
    /// later, and is deliberately not v1.
    func setOverlay(_ on: Bool) {
        overlay = on
        Prefs.overlay = on
        overlayPendingRestart = Shell.isRunning(ShimPath.steamOsx)
    }

    func restartSteam() {
        perform("Restarting Steam…") {
            Shell.run("/usr/bin/osascript", ["-e", "quit app \"Steam\""])
            for _ in 0..<30 where Shell.isRunning(ShimPath.steamOsx) {
                Thread.sleep(forTimeInterval: 0.5)
            }
            _ = Launch.spawn(overlay: Prefs.overlay)
            return ""
        }
        overlayPendingRestart = false
    }

    /// Uninstall is the deploy module's, for the same reason verify is: it is
    /// the half that wrote the receipt listing what to remove. The bottle is
    /// asked about separately because it is the user's, may hold save games,
    /// and was very likely not created by us.
    func uninstall(deleteBottle: Bool) {
        perform("Removing…") {
            var out = Shell.run(Receipt.deployScript, ["--uninstall"]).output
            if deleteBottle {
                try? FileManager.default.removeItem(atPath: Preflight.bottlePath)
                out += "\nremoved bottle \(Preflight.bottleName)"
            }
            Prefs.forget()
            return out
        }
        // The bundle this process is running from has just been deleted. There
        // is nothing left to show, so leaving a window up would be a lie.
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { NSApplication.shared.terminate(nil) }
    }

    private func perform(_ label: String, _ work: @escaping () -> String) {
        busy = label
        Task.detached {
            _ = work()
            await MainActor.run {
                self.busy = nil
                self.refresh()
            }
        }
    }
}
