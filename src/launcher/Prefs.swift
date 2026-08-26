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

    private static let overlayKey = "overlay"
    private static let firstRunKey = "firstRunCompleted"
    private static let verifiedKey = "verifiedVersion"

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
    /// owed something on screen.
    static var firstRunCompleted: Bool {
        get { store.bool(forKey: firstRunKey) }
        set { store.set(newValue, forKey: firstRunKey) }
    }

    /// The version that self-verification last passed for. A payload update is
    /// a new claim: it re-verifies rather than trusting the previous one.
    static var verifiedVersion: String? {
        get { store.string(forKey: verifiedKey) }
        set { store.set(newValue, forKey: verifiedKey) }
    }

    static func forget() {
        for key in [overlayKey, firstRunKey, verifiedKey] { store.removeObject(forKey: key) }
    }
}
