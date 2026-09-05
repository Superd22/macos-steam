import Foundation

/// Valve's own signed `steamclient64.dll`, and the one job in this app that
/// needs the network (ADR 0014, #105).
///
/// It is the single file of the DRM route that we do not ship and cannot: the
/// wrapper reads whichever file provided `CreateInterface` and verifies Valve's
/// signature over its bytes, and we are not allowed to pass Valve's bytes on.
/// So the app goes and gets it, from Valve's own public client manifest, to
/// this machine — which is what `src/drm/fetch.sh` does, and until #105 nothing
/// ever called it. Every caller here runs that one script rather than learning
/// to talk to Valve's manifest itself: it is the half that knows the endpoint,
/// verifies the published SHA-256, and checks our shadows against the client
/// build the user will actually run.
///
/// The cheap/expensive split is the cache, and it already existed. This is the
/// side that fills it, once; the launch script copies out of it into the bottle
/// per launch and skips even that when the sizes match. So "is the route armed"
/// is a stat, and only a cold machine pays for the download.
enum ValveClient {
    /// Beside the versions rather than inside one, so a deploy does not throw
    /// away 60 MB the user already paid for.
    static var cachedDLL: String { ShimPath.inHome(ShimPath.clientDllRel) }
    static var fetcher: String { ShimPath.inHome(ShimPath.fetchShRel) }

    /// The whole check, and cheap enough for `Preflight` to ask on every
    /// launch: one `stat`, no hashing and no network. Whether the cached copy
    /// is still the one this Steam client wants is `fetch.sh`'s question — it
    /// re-reads the manifest and compares Valve's published SHA-256 — and
    /// asking it costs a round trip, which is why it is never asked here.
    static var isCached: Bool { FileManager.default.fileExists(atPath: cachedDLL) }

    /// The id both the preflight row and the Diagnose row carry, so the pane
    /// that showed the button knows which row the failure belongs in.
    static let rowID = "valveClient"

    /// Why a game launcher is downloading 60 MB of Steam. Asking for that
    /// without a reason reads badly, and the reason is not embarrassing: it is
    /// the same sentence `fetch.sh`'s header opens with.
    static let why = "Steam's copy protection checks Valve's signature on Valve's own "
                   + "client file, and nobody but Valve may pass that file on — so it "
                   + "comes to you from Valve, about 60 MB, once per Steam client update."

    /// What `fetch.sh` prefixes its own lines with, as opposed to the output
    /// of the checker it calls.
    private static let narration = "drm-fetch: "

    enum Outcome: Equatable {
        case ok             // fetched, or already current — the route is armed
        case failed(String) // a sentence for the row that offered the button
    }

    /// Run the fetcher and watch it talk. `progress` gets each line it prints,
    /// which is what turns "the app is thinking" into "downloading 60 MB from
    /// Valve" — the script already narrates its three slow steps, so nothing
    /// here has to estimate anything.
    ///
    /// Blocking, and never to be called on the main thread.
    static func fetch(progress: @escaping (String) -> Void) -> Outcome {
        guard FileManager.default.isExecutableFile(atPath: fetcher) else {
            return .failed("The downloader is missing from this install. Reinstall to put it back.")
        }
        // Recorded before it runs, and whatever happens next: what this
        // timestamp gates is the UNATTENDED retry, so a failure has to count.
        Prefs.lastClientFetch = Date()
        var last = ""
        // Longer than `fetch.sh`'s own `curl --max-time 900`, on purpose: the
        // script owns the bound on the download (it is the half that knows the
        // endpoint), and a launcher bound that fired first would kill a fetch
        // that was still within its own budget. This is the backstop for the
        // case the script cannot cover — a curl that is neither transferring
        // nor timing out — and for a future caller that brings no bound (#108).
        let result = Shell.stream(fetcher, [], idleTimeout: 960) { line in
            // Its own narration only. `drm-fetch:` is a prefix for a log, not
            // for a label under a spinner — and the lines WITHOUT it come from
            // the shadow coverage checker it calls, which reports in full paths
            // and import counts. True, useful in a terminal, and not a sentence
            // to put in front of a user.
            guard line.hasPrefix(narration) else { return }
            let text = String(line.dropFirst(narration.count))
            guard !text.isEmpty else { return }
            last = text
            progress(text)
        }
        if result.ok { return .ok }
        if result.timedOut {
            return .failed("The download stopped responding and was cancelled. Try again — "
                         + "everything else works without this; only copy-protected games "
                         + "need it.")
        }
        return .failed(explain(result.status, last: last))
    }

    /// Start it and walk away — the pre-launch call site, where the user asked
    /// for Steam and not for a download. Orphaning it is deliberate: this
    /// process is about to `execve` into `steam_osx`, and the fetch outlives
    /// that. Nothing reports the result, because there is nobody to report it
    /// to; the next time a window opens, the two rows say where it got to.
    static func fetchDetached() {
        Prefs.lastClientFetch = Date()
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: fetcher)
        // Its own log lines are the only trace, and there is no window to put
        // them in. /dev/null rather than a pipe: a pipe with no reader fills
        // and stops the download at 64 KB.
        proc.standardOutput = FileHandle.nullDevice
        proc.standardError = FileHandle.nullDevice
        try? proc.run()
    }

    /// Whether the unattended path should try at all. It exists because the
    /// failure it guards against is silent and repeating: with no network,
    /// every single Steam launch would otherwise spawn a curl that cannot
    /// succeed, and with a network that drops mid-download it would re-spend
    /// 60 MB each time. A user who clicks the button is asking explicitly and
    /// is never throttled.
    static var mayFetchUnattended: Bool {
        guard ShimPolicy.shimDrmEnabled(), !isCached,
              FileManager.default.isExecutableFile(atPath: fetcher) else { return false }
        guard let last = Prefs.lastClientFetch else { return true }
        return Date().timeIntervalSince(last) > 24 * 60 * 60
    }

    /// `fetch.sh`'s exit codes, as sentences. A non-zero exit is not an answer
    /// a user can act on, and the four failures are four different situations:
    /// one is their network, one is Valve moving something, one is a file that
    /// arrived wrong, and one is us being out of date while the files are in
    /// fact installed.
    private static func explain(_ status: Int32, last: String) -> String {
        switch status {
        case 3:
            return "Could not reach Valve. Check your internet connection and try again. "
                 + "Everything else works without this; only copy-protected games need it."
        case 4:
            return "What arrived from Valve did not match what Valve says it should be, "
                 + "so it was thrown away. Try again later."
        case 5:
            // The DLLs are on disk and this is a coverage failure against THIS
            // client build (fetch.sh's check_shadow.py). Saying "failed" would
            // send the user to retry a download that already succeeded.
            return "Valve's file is here, but this Steam client build needs a newer "
                 + "version of this app before copy-protected games can use it."
        case 2, 127:
            return "The downloader is missing from this install. Reinstall to put it back."
        default:
            return last.isEmpty ? "The download did not finish." : last
        }
    }
}
