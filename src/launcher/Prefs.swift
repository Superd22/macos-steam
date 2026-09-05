import Foundation

/// The launcher's stored preferences.
///
/// One of them, the overlay, used to be baked into the launcher at install time
/// — which made "turn the overlay off" a reinstall. ADR 0006's single owner is
/// unchanged: the RULE still lives in the generated predicate and nothing here
/// re-derives it. What moved is only where the value comes from, from a literal
/// written into a script to a preference read at launch (#42).
///
/// Both binaries in the bundle read one suite, named explicitly rather than
/// taken from the bundle id: the nested settings helper has an id of its own,
/// and a settings pane writing to a store the launcher never reads is a toggle
/// that silently does nothing.
enum Prefs {
    static let store = UserDefaults(suiteName: ShimPath.prefsDomain) ?? .standard

    private static let overlayKey = ShimPath.prefOverlay
    private static let firstRunKey = "firstRunCompleted"

    /// Unset means "whatever the manifest says", asked of the predicate — never
    /// hardcoded here as `true`. An inherited environment can still state the
    /// answer, so a `SHIM_OVERLAY=0 open -a ...` keeps working the way it does
    /// everywhere else in the stack.
    static var overlay: Bool {
        get {
            if let stored = store.object(forKey: overlayKey) as? Bool { return stored }
            return ShimPolicy.shimOverlayEnabled()
        }
        set { store.set(newValue, forKey: overlayKey) }
    }

    /// True once the app has watched a launch reach `patched 1 site(s)` itself.
    /// Until then every launch goes through the checklist, because an install
    /// that has never been proven to work is exactly the case where a user is
    /// owed something on screen. After it, whether the mechanism still works is
    /// a live question the log answers (LogWatch.lastLaunchPatched) rather than
    /// a stored verdict that a payload update would invalidate.
    static var firstRunCompleted: Bool {
        get { store.bool(forKey: firstRunKey) }
        set { store.set(newValue, forKey: firstRunKey) }
    }

    /// When the unattended fetch of Valve's client DLL last ran (#105).
    ///
    /// Stored for one reason: the pre-launch call site starts that download
    /// with nobody watching, and a machine with no network would otherwise
    /// spawn a doomed curl on every single Steam launch, forever, saying
    /// nothing. A day between silent attempts is enough to arm a machine that
    /// comes back online without turning a network outage into a background
    /// job that never stops. A user pressing the button in either pane is
    /// asking explicitly and is never throttled by this.
    static var lastClientFetch: Date? {
        get { store.object(forKey: clientFetchKey) as? Date }
        set { store.set(newValue, forKey: clientFetchKey) }
    }

    private static let clientFetchKey = "lastClientFetch"

    static func forget() {
        for key in [overlayKey, firstRunKey, clientFetchKey] { store.removeObject(forKey: key) }
    }
}
