import AppKit
import SwiftUI

// The launcher (#42). Vehicle A of ADR 0002, as a Mach-O rather than the shell
// script that shipped before it.
//
// PRIME RULE: the happy path shows no UI. Clicking the icon behaves exactly
// like clicking Valve's own Steam.app — a preflight measured in stat calls, and
// then an exec. "Already running" needs no handling: Valve's single-instance
// logic forwards the second launch to the running client and focuses it, so
// re-clicking the Dock icon means *focus Steam*, the same muscle memory as
// before.
//
// UI appears in exactly three cases:
//   --settings (or the nested helper app)  the settings / diagnose / uninstall pane
//   ⌥ held at launch                        the same pane, without Spotlight
//   preflight broken, or nothing proven yet the checklist
//
// main.swift and not @main: top-level code is only legal in a file with this
// name, and the exec path must be able to leave before AppKit is ever brought
// up as an application.

let arguments = Array(CommandLine.arguments.dropFirst())

/// Arguments meant for us, not for Steam. Everything else is passed through:
/// Steam has its own command line, and LaunchServices hands us `-psn_…` on some
/// launch paths, which Valve's binary has always tolerated.
// Two ways in, one pane: the flag, and the nested helper app, which is the same
// binary in a bundle of its own. It identifies itself by ITS bundle id rather
// than by an argument, because LaunchServices gives a bundle no way to add one.
let launchedAsSettingsHelper = Bundle.main.bundleIdentifier == ShimPath.settingsId
let wantsSettings = arguments.contains(ShimPath.settingsFlag) || launchedAsSettingsHelper
let passthrough = arguments.filter { $0 != ShimPath.settingsFlag }

/// Reading the modifier state does not start an event loop or show anything —
/// it is a hardware query — so the exec path stays UI-free even though it asks.
let optionHeld = NSEvent.modifierFlags.contains(.option)

// Two flags that answer a question and exit, before anything else happens.
// Neither shows UI: one is what a user pastes into an issue, and the other is
// what check_launch_env.sh asks so that #85's acceptance test — both entries
// present, ours first, no duplicate on a second launch — can be run without
// launching Steam at all.
if arguments.contains(ShimPath.diagnoseFlag) {
    print(Diagnose.report())
    exit(0)
}
if arguments.contains(ShimPath.printEnvFlag) {
    let env = Launch.childEnvironment(overlay: Prefs.overlay)
    for key in [Launch.dyldInsertLibraries, Launch.extraCompatToolsPaths,
                Launch.compatToolMappings, ShimPolicy.envOverlay] {
        print("\(key)=\(env[key] ?? "")")
    }
    exit(0)
}

// A first run has nothing to be retrospective about: no launch has happened, so
// the checklist is how the very first one gets watched. Every launch after that
// is governed by preflight alone — "did the last launch open the gate" is one
// of its checks, and a blocked verdict is the whole answer.
// A client already up with Steam Play on is proof, so a user whose Steam is
// running does not get a checklist asking them to prove it again — clicking the
// icon focuses Steam, which is what clicking it always means.
let liveProof = LogWatch.gatePatchedSinceSteamStarted()
if !Prefs.firstRunCompleted && liveProof {
    Prefs.firstRunCompleted = true
}
let neverLaunched = !Prefs.firstRunCompleted
let checks = Preflight.run(liveProof: liveProof)

if !wantsSettings && !optionHeld && !neverLaunched && Preflight.isClear(checks) {
    // Before Steam starts, arm the DRM route if it is not armed (#105, call
    // site 3). On every launch but the first this is the stat preflight has
    // just done and nothing happens: the cache is filled once and sits beside
    // the versions, so it survives every deploy after it.
    //
    // Started and left to run, never waited for. The click means "start
    // Steam", and a 60 MB download in front of that is this app doing the one
    // thing its prime rule forbids. The fetcher outlives the exec below —
    // launchd adopts it — and the two rows in the windows report where it got
    // to, next time a window opens. Throttled, because nobody is watching:
    // see Prefs.lastClientFetch.
    if ValveClient.mayFetchUnattended { ValveClient.fetchDetached() }
    Launch.exec(passthrough: passthrough, overlay: Prefs.overlay)   // never returns
}

// From here we are an app with a window. Regular activation policy: the
// checklist is a window a user is meant to find in the Dock and come back to.
let pane: RootView.Pane = (wantsSettings || optionHeld) ? .settings : .checklist

final class Delegate: NSObject, NSApplicationDelegate {
    let pane: RootView.Pane
    var window: NSWindow!

    init(pane: RootView.Pane) { self.pane = pane }

    func applicationDidFinishLaunching(_ note: Notification) {
        window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: Metrics.width, height: Metrics.height),
            styleMask: [.titled, .closable, .miniaturizable, .fullSizeContentView],
            backing: .buffered, defer: false)
        // No titlebar of its own: the content starts at the top of the window
        // and leaves room for the traffic lights. Both panes already say what
        // they are in their first line, so a title bar would only repeat it.
        window.titlebarAppearsTransparent = true
        window.titleVisibility = .hidden
        window.isMovableByWindowBackground = true
        // Come to whichever desktop the user is on. Clicking the icon is a
        // request to see this window now, not a request to be sent to the
        // desktop it happened to open on three hours ago.
        window.collectionBehavior = [.moveToActiveSpace]
        // The title is still set, because it is what the Window menu and
        // Mission Control show.
        window.title = pane == .settings ? "Steam Play Settings" : "Steam (macOS Play)"
        window.contentView = NSHostingView(rootView: RootView(pane: pane))
        // Fixed size: the layout is a column at one width, and a resizable
        // window would only offer the user worse versions of it.
        window.setContentSize(NSSize(width: Metrics.width, height: Metrics.height))
        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    /// Closing the only window means done — there is nothing running behind it.
    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { true }
}

let delegate = Delegate(pane: pane)
let app = NSApplication.shared
app.setActivationPolicy(.regular)
app.delegate = delegate
app.run()
