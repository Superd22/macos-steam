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
    case createBottle
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
    /// on every launch: six stats and one small file read.
    static func run() -> [Check] {
        [system(), crossover(), steam(), bottle(), payload(), selfVerification()]
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
            return Check(id: "system", title: "Apple Silicon Mac",
                         verdict: .blocked,
                         detail: "This Mac is Intel. The compat-gate patch is arm64 code; there is no Intel path.")
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

    // 2 — CrossOver. The Windows title runs in its bottle; without it there is
    // no Level B at all.
    static func crossover() -> Check {
        let app = ShimPath.inHome(ShimPath.cxAppRel)
        guard FileManager.default.fileExists(atPath: app) else {
            return Check(id: "crossover", title: "CrossOver installed",
                         verdict: .blocked,
                         detail: "Windows games run inside a CrossOver bottle. Install CrossOver, then reopen this window.",
                         remedy: .openURL(URL(string: "https://www.codeweavers.com/crossover")!),
                         remedyTitle: "Get CrossOver")
        }
        let version = Bundle(path: app)?.infoDictionary?["CFBundleShortVersionString"] as? String
        return Check(id: "crossover", title: "CrossOver \(version ?? "installed")",
                     verdict: .ok, detail: "")
    }

    // 3 — Valve's own client. We never modify it; we exec it.
    static func steam() -> Check {
        guard FileManager.default.isExecutableFile(atPath: Launch.steamOsx) else {
            return Check(id: "steam", title: "Steam installed",
                         verdict: .blocked,
                         detail: "The native macOS Steam client is what this launcher starts. Install Steam first.",
                         remedy: .openURL(URL(string: "https://store.steampowered.com/about/")!),
                         remedyTitle: "Get Steam")
        }
        guard signedIn else {
            return Check(id: "steam", title: "Signed in to Steam",
                         verdict: .warning,
                         detail: "No stored login found. Steam Play needs an online, signed-in client to install a Windows depot.")
        }
        return Check(id: "steam", title: "Steam installed and signed in", verdict: .ok, detail: "")
    }

    private static var signedIn: Bool {
        // Valve's own record of accounts that have logged in on this machine.
        // Presence is the claim being made — "you will not be asked to sign in
        // from scratch" — not that the client is online this second, which
        // nothing outside the client can honestly answer.
        let vdf = ShimPath.inHome("Library/Application Support/Steam/config/loginusers.vdf")
        guard let text = try? String(contentsOfFile: vdf, encoding: .utf8) else { return false }
        return text.contains("\"AccountName\"")
    }

    // 4 — a clean win10_64 bottle. "Clean" means no Windows Steam in it: a real
    // steamclient64.dll would win the lookup against our shim, which is the
    // whole reason the bottle is ours and not any bottle.
    static var bottleName: String {
        ProcessInfo.processInfo.environment["SHIM_BOTTLE"] ?? ShimPath.bottleDefault
    }
    static var bottlePath: String {
        ShimPath.inHome(ShimPath.cxBottlesRel) + "/" + bottleName
    }

    static func bottle() -> Check {
        let fm = FileManager.default
        guard fm.fileExists(atPath: bottlePath) else {
            return Check(id: "bottle", title: "Bottle “\(bottleName)”",
                         verdict: .blocked,
                         detail: "The Windows-side container does not exist yet. This app can create it — a clean win10_64 bottle with no Windows Steam in it.",
                         remedy: .createBottle,
                         remedyTitle: "Create it")
        }
        let windowsSteam = ["drive_c/Program Files (x86)/Steam/steam.exe",
                            "drive_c/Program Files/Steam/steam.exe"]
            .first { fm.fileExists(atPath: bottlePath + "/" + $0) }
        if let stray = windowsSteam {
            return Check(id: "bottle", title: "Bottle “\(bottleName)” is not clean",
                         verdict: .blocked,
                         detail: "A Windows Steam is installed in this bottle (\(stray)). Its \(ShimPath.pe64) wins the lookup against ours, so titles talk to it instead of your Mac's Steam. Remove it, or point SHIM_BOTTLE at a clean bottle.")
        }
        return Check(id: "bottle", title: "Bottle “\(bottleName)” ready", verdict: .ok, detail: "")
    }

    /// CodeWeavers' own tool, with the flags the README used to have to teach.
    static func createBottle() -> Shell.Result {
        let cxbottle = ShimPath.inHome(ShimPath.cxAppRel) + "/Contents/SharedSupport/CrossOver/bin/cxbottle"
        return Shell.run(cxbottle, ["--create", "--bottle", bottleName, "--template", "win10_64"], timeout: 300)
    }

    // 5 — the payload, through `current`. Existence only: whether the bytes are
    // the bytes that shipped is the receipt's question, and asking it costs a
    // hash of every file.
    static func payload() -> Check {
        let fm = FileManager.default
        guard let receipt = Receipt.load() else {
            return Check(id: "payload", title: "Steam Play payload",
                         verdict: .blocked,
                         detail: "Nothing is deployed — there is no receipt. Reinstall to lay the payload down.",
                         remedy: .reinstall, remedyTitle: "How to reinstall")
        }
        let missing = [Launch.enabler, Launch.compatTools].filter { !fm.fileExists(atPath: $0) }
        guard missing.isEmpty else {
            return Check(id: "payload", title: "Steam Play payload",
                         verdict: .blocked,
                         detail: "The receipt says \(receipt.version) is deployed, but \(missing.count) part(s) of it are not on disk. Reinstall to repair.",
                         remedy: .reinstall, remedyTitle: "How to reinstall")
        }
        return Check(id: "payload", title: "Payload \(receipt.version) deployed",
                     verdict: .ok, detail: "")
    }

    // 6 — the one check that cannot be made before a launch. The user never
    // reads a log for `patched 1 site(s)`; the app reads it and says "ready".
    static func selfVerification() -> Check {
        // Proven once, and still working as of the last launch. The second half
        // is what makes this a live check rather than a stored opinion, and it
        // is why an update does not have to re-ask interactively.
        if Prefs.firstRunCompleted && LogWatch.lastLaunchPatched() {
            return Check(id: "verified", title: "Compat gate proven on this Mac",
                         verdict: .ok, detail: "")
        }
        if Prefs.firstRunCompleted {
            return Check(id: "verified", title: "The last launch did not open the compat gate",
                         verdict: .blocked,
                         detail: "It has worked here before, so something changed — an updated Steam client, or a launch that came up translated as x86_64. Start Steam from here and this window will report what happens.")
        }
        return Check(id: "verified", title: "Compat gate not proven yet",
                     verdict: .pending,
                     detail: "The gate is flipped in memory at each launch. Start Steam from here and this window will confirm it worked.")
    }
}
