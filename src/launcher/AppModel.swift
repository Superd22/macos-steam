import SwiftUI

/// What the two panes share: the preflight verdicts, the first-run launch that
/// proves itself, and the long-running actions (verify, bottle creation,
/// uninstall) that must not run on the main thread.
@MainActor
final class AppModel: ObservableObject {
    @Published var checks: [Check] = Preflight.run()
    /// Whether a client is up that we did not switch Steam Play on for. Stored
    /// rather than asked: answering it costs a `pgrep` and a `ps`, and a view
    /// body must never spawn a process — it runs on every render, and the
    /// mutation it invites lands inside a SwiftUI update.
    @Published var steamNeedsRestart = false
    @Published var findings: [Diagnose.Finding] = []
    @Published var busy: String?
    @Published var launchState: LaunchState = .idle
    @Published var overlay: Bool = Prefs.overlay
    @Published var overlayPendingRestart = false
    /// Why the last fetch of Valve's client DLL did not work (#105). Kept on
    /// the model rather than in the row, because the row is recomputed from
    /// disk — after a failed download the file is still absent, so the check
    /// would go back to the generic "copy-protected games need a file from
    /// Valve" and the user would press the same button expecting a different
    /// result. This is what the row says instead, until something changes it.
    @Published var valveClientProblem: String?

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

    /// Everything the two panes read, recomputed off the main thread and
    /// published in one go. Both halves matter: the subprocess work must not
    /// block a render, and the state change must not land in the middle of one.
    func refresh() {
        Task.detached(priority: .userInitiated) {
            let live = LogWatch.gatePatchedSinceSteamStarted()
            let running = Shell.isRunning(ShimPath.steamOsx)
            let checks = Preflight.run(liveProof: live)
            await MainActor.run {
                // A Steam already up with Steam Play on has proven the thing the
                // first run exists to prove. Record it and say so, rather than
                // asking for a restart to be told what is already true.
                if live {
                    Prefs.firstRunCompleted = true
                    if self.launchState == .idle { self.launchState = .ready(sites: 1) }
                }
                self.checks = checks
                self.steamNeedsRestart = running && !live
            }
        }
    }

    /// Re-check is a few stat calls, so it finishes before the pointer leaves
    /// the button and nothing on screen moves. The pause and the spinner are
    /// the acknowledgement: a control that answers invisibly reads as broken.
    func recheck() {
        busy = "Checking…"
        Task {
            try? await Task.sleep(nanoseconds: 400_000_000)
            self.refresh()
            self.busy = nil
        }
    }

    /// Kick work off *after* the update that asked for it has finished. Calling
    /// straight from `onAppear` mutates state the current view update is still
    /// reading, which SwiftUI treats as a cycle and aborts on.
    func refreshSoon() {
        Task { @MainActor in self.refresh() }
    }

    func diagnoseSoon() {
        Task { @MainActor in
            guard self.busy == nil else { return }
            self.runDiagnose()
        }
    }

    /// The first-run launch, and the only launch this app does not exec into.
    /// Staying alive is the whole point: it is what lets the app read
    /// `patched 1 site(s)` itself and tell the user, rather than sending them
    /// to a log file — check 6 of the preflight list.
    /// True when a client is already up that we did not switch Steam Play on
    /// for. Starting Steam again in that state proves nothing: Valve's
    /// single-instance handling forwards the second launch to the running
    /// client, so the injector runs in a process that immediately exits.
    var mustQuitSteamFirst: Bool { steamNeedsRestart }

    /// The first-run launch is also where the DRM route gets armed (#105, call
    /// site 1). A machine that has run this app once should be able to start a
    /// copy-protected game, and this is the only moment where a user is
    /// present, has just asked for something, and can be told what is
    /// happening — the pre-launch site cannot say a word, and Diagnose is
    /// somewhere most people never open.
    ///
    /// It never stops the launch. Offline, the fetcher gives up on the manifest
    /// in under a minute and Steam starts anyway, which is the correct outcome:
    /// everything that is not copy-protected works without any of this.
    func launchAndProve() {
        if ValveClient.mayFetchUnattended {
            Task.detached {
                await self.fetchValveClient(then: { self.launchAfterFetch() })
            }
            return
        }
        launchAfterFetch()
    }

    private func launchAfterFetch() {
        if mustQuitSteamFirst {
            busy = "Quitting Steam…"
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
            launchState = .launchedButUnproven("Steam would not start.")
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
                    "Steam started, but Steam Play did not switch on. Quit Steam and try again from here.")
            case .timedOut, .none:
                self.launchState = .launchedButUnproven(
                    "Steam started, but Steam Play did not switch on. Diagnose has more.")
            }
        }
    }

    /// The download, with the fetcher's own narration as the busy label (#105).
    /// Every call site ends up here: the checklist's button, Diagnose's button,
    /// and the first run. Detached, because it blocks for as long as 60 MB
    /// takes; `then` runs on the main actor once it is over, whatever the
    /// outcome, which is what lets the first run carry on into the launch
    /// rather than stopping at a failed download.
    nonisolated func fetchValveClient(then next: (@MainActor () -> Void)? = nil) async {
        await MainActor.run {
            self.busy = "Getting Valve's client file…"
            self.valveClientProblem = nil
        }
        let outcome = ValveClient.fetch { line in
            Task { @MainActor in
                // Only while this is still the thing on screen: a line arriving
                // after the user cancelled into something else must not
                // repaint somebody else's label.
                if self.busy != nil { self.busy = line }
            }
        }
        await MainActor.run {
            self.busy = nil
            if case .failed(let why) = outcome { self.valveClientProblem = why }
            self.refresh()
            // Diagnose's list is a snapshot, so the row that offered the button
            // is stale the moment the button worked — but only re-run it if the
            // pane is actually showing findings.
            if !self.findings.isEmpty { self.runDiagnose() }
            next?()
        }
    }

    func createBottle() {
        perform("Making the bottle…") { Preflight.createBottle().output }
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
        // "Is Steam up?" is a subprocess, and this runs from a Toggle's setter
        // during an update. Answer it after.
        Task.detached(priority: .userInitiated) {
            let running = Shell.isRunning(ShimPath.steamOsx)
            await MainActor.run { self.overlayPendingRestart = running }
        }
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
