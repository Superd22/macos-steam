import Foundation

/// Handing off to Valve's own `steam_osx`, unmodified — the only thing this app
/// does on the happy path (#42, ADR 0002 vehicle A).
///
/// Three variables have to reach that process and nothing else does:
/// `DYLD_INSERT_LIBRARIES` (the compat-gate injector),
/// `STEAM_EXTRA_COMPAT_TOOLS_PATHS` (our tool dir, outside every bundle so a
/// client update cannot touch it) and `STEAM_COMPAT_TOOL_MAPPINGS` (the
/// wildcard mapping — the gate says compat runs, this says which tool it should
/// reach for, and neither half is any use alone, ADR 0015). A fourth, the
/// overlay switch, is stated so a compat tool started by Steam inherits an
/// answer rather than a default.
enum Launch {
    static var steamOsx: String { ShimPath.inHome(ShimPath.steamOsxRel) }
    static var enabler: String { ShimPath.inHome(ShimPath.enablerRel) }
    static var compatTools: String { ShimPath.inHome(ShimPath.compatToolsRel) }

    /// The wildcard mapping (ADR 0015): one entry, `<appid> <priority> <tool>`,
    /// naming the tool `compatTools` above delivers. It is the standing answer
    /// to "which tool should a title use when nobody has chosen one for it",
    /// and without it the client reports every Windows-only title as unavailable
    /// on this platform — registering a tool declares it willing, it does not
    /// select it.
    static var toolMappings: String {
        "\(wildcardAppID) \(wildcardPriority) \(ShimPath.toolName)"
    }

    /// Valve's "every app" sentinel in a compat tool mapping (`k_nAppIdAll`).
    private static let wildcardAppID = 0

    /// The client honours a wildcard mapping only at 250 or above; below that it
    /// stores the entry and never consults it. Stated as the threshold itself
    /// rather than a comfortable number above it, so the next reader can match
    /// it against the client and see that it is a boundary and not a taste.
    private static let wildcardPriority = 250

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
        env[compatToolMappings] = toolMappings
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
    static let compatToolMappings = "STEAM_COMPAT_TOOL_MAPPINGS"
}

/// `execve` wants a NULL-terminated array of C strings, and Swift will not keep
/// the backing buffers alive past the expression that made them.
private func withCStrings<R>(_ strings: [String], _ body: ([UnsafeMutablePointer<CChar>?]) -> R) -> R {
    var pointers = strings.map { strdup($0) }
    pointers.append(nil)
    defer { for p in pointers where p != nil { free(p) } }
    return body(pointers)
}
