import Foundation

/// The millisecond check that runs before every launch, and the checklist the
/// user sees when it fails (#42).
///
/// The prime rule is that the happy path shows no UI, so this is stat calls and
/// nothing else — no hashing, no subprocess, no network. Hashing every deployed
/// file is what `deploy.sh --verify` is for, and it lives in Diagnose where a
/// user has asked a question and can wait for the answer.
///
/// Each verdict is written as something a user can act on. "Install CrossOver
/// first" is a sentence; a path to a log is not.
enum Verdict: Equatable {
    case ok           // proven, right now
    case blocked      // Steam will start but the thing the app exists for will not work
    case warning      // worth saying, not worth stopping for
    case pending      // cannot be known until Steam has been launched once
}

enum Remedy: Equatable {
    case openURL(URL)
    /// Launch a bundle already on disk, by path. Distinct from `openURL` on a
    /// file URL, which would do the same thing while reading as "go and get
    /// this": the only check that offers it is the one whose whole point is
    /// that the app is already there.
    case openApp(String)
    case createBottle
    /// Run the DRM module's downloader, with its progress on screen (#105).
    /// The other cases hand the job to somebody else and are done in a frame;
    /// this one is ours, takes a minute, and can fail for a reason the user has
    /// to be told — which is why it is a case here rather than an `openURL` at
    /// a page explaining how to run a shell script.
    case fetchValveClient
    case reinstall
    case none
}

struct Check: Identifiable, Equatable {
    let id: String
    let title: String
    let verdict: Verdict
    let detail: String
    var remedy: Remedy = .none
    var remedyTitle: String = ""
}

enum Preflight {
    /// The whole list, in the order the checklist shows it. Cheap enough to run
    /// on every launch: seven stats and one small file read.
    ///
    /// Deliberately free of subprocesses. `liveProof` — whether a Steam that is
    /// up right now has Steam Play on — costs a `ps` and a `pgrep`, so the
    /// caller establishes it off the main thread and passes it in. Asking it
    /// here would put a process spawn inside a SwiftUI view update, which is
    /// both slow on every render and how this crashed once already.
    static func run(liveProof: Bool = false) -> [Check] {
        var list = [system(), crossover(), steam(), bottle(), payload()]
        // Only when the route it serves is switched on. With SHIM_DRM=0 the
        // launch script will not take that route however well armed it is, so a
        // row about a missing download would be asking for 60 MB to satisfy a
        // check that nothing reads.
        if ShimPolicy.shimDrmEnabled() { list.append(valveClient()) }
        list.append(selfVerification(liveProof: liveProof))
        return list
    }

    /// The question the happy path asks. A blocked check means the user clicked
    /// our icon and would have got a Steam that cannot do the one thing this
    /// app is for — which is the one case where uninvited UI is the kinder
    /// answer.
    static func isClear(_ checks: [Check]) -> Bool {
        !checks.contains { $0.verdict == .blocked }
    }

    // 1 — Apple Silicon, macOS >= 14.
    // Both are hard requirements of the mechanism, not preferences: the
    // injector's gate pattern is arm64 code, and the exec inherits the arch
    // (ADR 0002's second correction).
    static func system() -> Check {
        let os = ProcessInfo.processInfo.operatingSystemVersion
        let version = "\(os.majorVersion).\(os.minorVersion)"
        guard isAppleSilicon else {
            return Check(id: "system", title: "Needs an Apple Silicon Mac",
                         verdict: .blocked,
                         detail: "Steam Play needs an Apple Silicon Mac. This one has an Intel chip.")
        }
        guard os.majorVersion >= 14 else {
            return Check(id: "system", title: "macOS 14 or later",
                         verdict: .blocked,
                         detail: "This Mac runs macOS \(version).")
        }
        return Check(id: "system", title: "Apple Silicon, macOS \(version)",
                     verdict: .ok, detail: "")
    }

    private static var isAppleSilicon: Bool {
        var value: Int32 = 0
        var size = MemoryLayout<Int32>.size
        guard sysctlbyname("hw.optional.arm64", &value, &size, nil, 0) == 0 else { return false }
        return value == 1
    }

    /// Where CrossOver actually is. Resolved rather than assumed: this used to
    /// be ~/Applications only, so it reported "not installed" to anyone who had
    /// dragged CrossOver into /Applications — which is what its own DMG tells
    /// you to do — and no amount of re-checking could ever clear it.
    static var crossoverApp: String { ShimPath.installedApp(ShimPath.cxAppRel) }

    // 2 — CrossOver. The Windows title runs in its bottle; without it there is
    // no Level B at all.
    static func crossover() -> Check {
        let app = crossoverApp
        guard FileManager.default.fileExists(atPath: app) else {
            return Check(id: "crossover", title: "CrossOver installed",
                         verdict: .blocked,
                         detail: "Windows games need CrossOver to run. Install it, then open this window again.",
                         remedy: .openURL(URL(string: "https://www.codeweavers.com/crossover")!),
                         remedyTitle: "Get CrossOver")
        }
        let version = Bundle(path: app)?.infoDictionary?["CFBundleShortVersionString"] as? String
        return Check(id: "crossover", title: "CrossOver \(version ?? "installed")",
                     verdict: .ok, detail: "")
    }

    // 3 — Valve's own client. We never modify it; we exec it.
    //
    // Presence only. Whether the user is signed in was checked here once and
    // pulled back out: it reads a file Valve owns to answer a question only the
    // running client can answer, and a signed-out Steam is a state Steam itself
    // explains far better than we can.
    static func steam() -> Check {
        guard FileManager.default.isExecutableFile(atPath: Launch.steamOsx) else {
            // Two states, not one. Valve's installer puts Steam.app down; the
            // bundle we exec is the one Steam's bootstrapper unpacks under
            // Application Support on its first successful run. So a machine
            // that has installed Steam and never opened it lands here with
            // Steam.app sitting right there — and "Install Steam first" over a
            // Get Steam button sends that user to re-download what they have,
            // which will not fix it however many times they do it.
            let steamApp = ShimPath.installedApp(ShimPath.steamAppRel)
            if FileManager.default.fileExists(atPath: steamApp) {
                return Check(id: "steam", title: "Steam has never been opened",
                             verdict: .blocked,
                             detail: "Steam is installed but has not finished its first run, "
                                   + "so the client this app starts does not exist yet. Open "
                                   + "Steam, sign in while online, let it update, then quit it "
                                   + "and come back here.",
                             remedy: .openApp(steamApp),
                             remedyTitle: "Open Steam")
            }
            return Check(id: "steam", title: "Steam installed",
                         verdict: .blocked,
                         detail: "This app starts your normal Steam. Install Steam first.",
                         remedy: .openURL(URL(string: "https://store.steampowered.com/about/")!),
                         remedyTitle: "Get Steam")
        }
        return Check(id: "steam", title: "Steam installed", verdict: .ok, detail: "")
    }

    // 4 — a clean win10_64 bottle. "Clean" means no Windows Steam in it: a real
    // steamclient64.dll would win the lookup against our shim, which is the
    // whole reason the bottle is ours and not any bottle.
    static var bottleName: String {
        ProcessInfo.processInfo.environment["SHIM_BOTTLE"] ?? ShimPath.bottleDefault
    }
    /// The id the bottle rows carry, so the pane that showed the "Create it"
    /// button knows which row a failure belongs in — the same arrangement
    /// `ValveClient.rowID` has.
    static let bottleRowID = "bottle"
    static var bottlePath: String {
        ShimPath.inHome(ShimPath.cxBottlesRel) + "/" + bottleName
    }

    static func bottle() -> Check {
        let fm = FileManager.default
        guard fm.fileExists(atPath: bottlePath) else {
            return Check(id: bottleRowID, title: "No CrossOver bottle yet",
                         verdict: .blocked,
                         detail: "Windows games run inside a CrossOver bottle. This app can make one for you.",
                         remedy: .createBottle,
                         remedyTitle: "Create it")
        }
        let hasWindowsSteam = ["drive_c/Program Files (x86)/Steam/steam.exe",
                               "drive_c/Program Files/Steam/steam.exe"]
            .contains { fm.fileExists(atPath: bottlePath + "/" + $0) }
        if hasWindowsSteam {
            return Check(id: bottleRowID, title: "The CrossOver bottle has a Windows Steam in it",
                         verdict: .blocked,
                         detail: "There is a Windows copy of Steam in this bottle. Games will talk to that instead of the Steam on your Mac. Delete it, or use a different bottle.")
        }
        // The bottle's name is a setting, not news. It appears where someone can
        // act on it (the uninstall toggle), not in a row whose job is "fine".
        return Check(id: bottleRowID, title: "CrossOver bottle configured", verdict: .ok, detail: "")
    }

    /// What a bottle that never finished says. The row is recomputed from disk
    /// and there is still no bottle there, so without this the checklist goes
    /// back to "This app can make one for you" and invites the same button and
    /// the same five-minute wait (#108).
    static let bottleTimedOut =
        "Making the bottle took more than five minutes and was stopped. Open CrossOver "
        + "once to let it finish setting itself up, then try again."

    /// CodeWeavers' own tool, with the flags the README used to have to teach.
    /// The five minutes is now a bound and not a hope: bottle creation is
    /// CodeWeavers' code, it touches the network on some paths, and it is
    /// offered as a button on the checklist — so a stall used to present as an
    /// app that had stopped responding (#108).
    static func createBottle() -> Shell.Result {
        let cxbottle = crossoverApp + "/Contents/SharedSupport/CrossOver/bin/cxbottle"
        return Shell.run(cxbottle, ["--create", "--bottle", bottleName, "--template", "win10_64"], timeout: 300)
    }

    // 5 — the payload, through `current`. Existence only: whether the bytes are
    // the bytes that shipped is the receipt's question, and asking it costs a
    // hash of every file.
    static func payload() -> Check {
        let fm = FileManager.default
        guard let receipt = Receipt.load() else {
            return Check(id: "payload", title: "Steam Play files are missing",
                         verdict: .blocked,
                         detail: "Reinstall to put them in place.",
                         remedy: .reinstall, remedyTitle: "How to reinstall")
        }
        let missing = [Launch.enabler, Launch.compatTools].filter { !fm.fileExists(atPath: $0) }
        guard missing.isEmpty else {
            return Check(id: "payload", title: "Some Steam Play files are missing",
                         verdict: .blocked,
                         detail: "Reinstall to put them back.",
                         remedy: .reinstall, remedyTitle: "How to reinstall")
        }
        // The version belongs in Settings, where someone is asking. On the
        // checklist it is noise in a row whose job is to say "fine".
        _ = receipt
        return Check(id: "payload", title: "Steam Play files ready", verdict: .ok, detail: "")
    }

    // 6 — Valve's own signed client DLL, the only thing in this list that is
    // not on the machine because we put it there (ADR 0014, #105). One stat, as
    // cheap as the five above it; the download itself hangs off the remedy,
    // which is what keeps this file free of the network.
    //
    // A warning and never blocked: the rest of the library runs without it, and
    // stopping a user whose games all work in order to demand 60 MB for the
    // ones they may not own would be this app inventing a problem. It is the
    // same interlock the payload already states for a missing shadow — deploys
    // fine, and leaves DRM-wrapped titles failing as they did before.
    static func valveClient() -> Check {
        guard ValveClient.isCached else {
            return Check(id: ValveClient.rowID,
                         title: "Copy-protected games need one file from Valve",
                         verdict: .warning,
                         detail: ValveClient.why,
                         remedy: .fetchValveClient,
                         remedyTitle: "Download it")
        }
        return Check(id: ValveClient.rowID, title: "Valve's client file is here",
                     verdict: .ok, detail: "")
    }

    // 7 — the one check that cannot be made before a launch. The user never
    // reads a log for `patched 1 site(s)`; the app reads it and says "ready".
    static func selfVerification(liveProof: Bool) -> Check {
        // A client that is up right now with Steam Play switched on is the
        // strongest possible answer, and it needs no cooperation from the user:
        // asking someone to quit a working Steam and start it again, to be told
        // what is already true, is the opposite of confirming it.
        if liveProof {
            return Check(id: "verified", title: "Steam Play is switched on",
                         verdict: .ok, detail: "")
        }
        // Proven once, and still working as of the last launch. The second half
        // is what makes this a live check rather than a stored opinion, and it
        // is why an update does not have to re-ask interactively.
        if Prefs.firstRunCompleted && LogWatch.lastLaunchPatched() {
            return Check(id: "verified", title: "Steam Play is switched on",
                         verdict: .ok, detail: "")
        }
        if Prefs.firstRunCompleted {
            return Check(id: "verified", title: "Steam Play did not switch on last time",
                         verdict: .blocked,
                         detail: "It has worked here before, so something changed. Start Steam from here and this window will show what happens.")
        }
        return Check(id: "verified", title: "Not tried yet",
                     verdict: .pending,
                     detail: "Start Steam from here once and this window will confirm it worked.")
    }
}
