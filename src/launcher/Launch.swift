import Foundation

/// Handing off to Valve's own `steam_osx`, unmodified — the only thing this app
/// does on the happy path (#42, ADR 0002 vehicle A).
///
/// Two variables have to reach that process and nothing else does:
/// `DYLD_INSERT_LIBRARIES` (the compat-gate injector) and
/// `STEAM_EXTRA_COMPAT_TOOLS_PATHS` (our tool dir, outside every bundle so a
/// client update cannot touch it). A third, the overlay switch, is stated so a
/// compat tool started by Steam inherits an answer rather than a default.
enum Launch {
    static var steamOsx: String { ShimPath.inHome(ShimPath.steamOsxRel) }
    static var enabler: String { ShimPath.inHome(ShimPath.enablerRel) }
    static var compatTools: String { ShimPath.inHome(ShimPath.compatToolsRel) }

    /// The environment `steam_osx` should be started with, derived from the one
    /// we were started with. Pure, so the acceptance test for the merge below
    /// is a function call and not a launched Steam.
    static func childEnvironment(
        from parent: [String: String] = ProcessInfo.processInfo.environment,
        overlay: Bool
    ) -> [String: String] {
        var env = parent
        env[dyldInsertLibraries] = merged(insertions: parent[dyldInsertLibraries], ours: enabler)
        env[extraCompatToolsPaths] = compatTools
        ShimPolicy.overlayExport(overlay, into: &env)
        return env
    }

    /// `DYLD_INSERT_LIBRARIES` is a ':'-separated list and we are one entry in
    /// it, not the owner of it (#85): prepend ours, keep whatever the user or
    /// another tool already inserted, and add ourselves only once.
    ///
    /// Membership is tested on RESOLVED paths — the same dylib spelled relative
    /// or reached through a symlink is still the same dylib, and comparing
    /// literals is how the list grows without bound across nested launches.
    /// Which is live here in a way it never was for the shell launcher it
    /// replaces: `/bin/sh` drops `DYLD_*` on the way in, so that script never
    /// saw an inherited value at all. A Mach-O does.
    static func merged(insertions: String?, ours: String) -> String {
        let existing = (insertions ?? "").split(separator: ":", omittingEmptySubsequences: true).map(String.init)
        let target = resolve(ours)
        if existing.contains(where: { resolve($0) == target }) {
            return existing.joined(separator: ":")   // already listed: leave it exactly as found
        }
        return ([ours] + existing).joined(separator: ":")
    }

    private static func resolve(_ path: String) -> String {
        URL(fileURLWithPath: path).resolvingSymlinksInPath().standardizedFileURL.path
    }

    /// Replace this process with Steam. Never returns on success — which is the
    /// point: the Dock icon, the app's identity and its architecture are all
    /// inherited across the exec, so from here on there is exactly one process
    /// and it is Valve's.
    static func exec(passthrough args: [String], overlay: Bool) -> Never {
        let env = childEnvironment(overlay: overlay)
        let argv = [steamOsx] + args
        _ = withCStrings(argv) { cargv in
            withCStrings(env.map { "\($0.key)=\($0.value)" }) { cenv in
                execve(steamOsx, cargv, cenv)
            }
        }
        // Only reachable if execve failed. There is no UI to fall back to here:
        // the caller ran preflight, so a failure now is not a state a checklist
        // has an entry for.
        FileHandle.standardError.write(
            Data("launcher: cannot exec \(steamOsx): \(String(cString: strerror(errno)))\n".utf8))
        exit(127)
    }

    /// Start Steam as a CHILD rather than becoming it. Used only on first run,
    /// where the app has to outlive the launch to watch for `patched 1 site(s)`
    /// and tell the user it worked — the one thing the exec path can never do.
    @discardableResult
    static func spawn(overlay: Bool) -> pid_t? {
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: steamOsx)
        proc.environment = childEnvironment(overlay: overlay)
        do { try proc.run() } catch { return nil }
        return proc.processIdentifier
    }

    // Valve's and dyld's variable names, not ours: the manifest owns the
    // contract this project defines, and borrowing someone else's name into it
    // would claim ownership of a spelling we cannot change.
    static let dyldInsertLibraries = "DYLD_INSERT_LIBRARIES"
    static let extraCompatToolsPaths = "STEAM_EXTRA_COMPAT_TOOLS_PATHS"
}

/// `execve` wants a NULL-terminated array of C strings, and Swift will not keep
/// the backing buffers alive past the expression that made them.
private func withCStrings<R>(_ strings: [String], _ body: ([UnsafeMutablePointer<CChar>?]) -> R) -> R {
    var pointers = strings.map { strdup($0) }
    pointers.append(nil)
    defer { for p in pointers where p != nil { free(p) } }
    return body(pointers)
}
