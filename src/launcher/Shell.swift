import Foundation

/// Running a command and keeping what it said.
///
/// The launcher shells out for exactly four things, all of them owned by
/// somebody else: `deploy.sh` (verify, uninstall, rollback — ADR 0010 says the
/// deploy module owns those, so reimplementing them here would be a second
/// implementation that can disagree), `fetch.sh` (the DRM module's downloader —
/// it owns Valve's manifest, the published SHA-256 and the shadow coverage
/// check, and none of that gets a second implementation either), `cxbottle`
/// (CodeWeavers' bottle creation, whose incantation this app exists to keep out
/// of the README), and `pgrep`.
enum Shell {
    struct Result {
        let status: Int32
        let output: String
        var ok: Bool { status == 0 }
    }

    @discardableResult
    static func run(_ path: String, _ args: [String], timeout: TimeInterval = 120) -> Result {
        guard FileManager.default.isExecutableFile(atPath: path) else {
            return Result(status: 127, output: "\(path): not executable")
        }
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: path)
        proc.arguments = args
        let pipe = Pipe()
        proc.standardOutput = pipe
        proc.standardError = pipe
        do { try proc.run() } catch {
            return Result(status: 127, output: "\(path): \(error.localizedDescription)")
        }
        // Read before waiting: a command that fills the pipe buffer while we
        // wait for it to exit is a deadlock, and `deploy.sh --verify` prints a
        // line per problem.
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        proc.waitUntilExit()
        return Result(status: proc.terminationStatus,
                      output: String(data: data, encoding: .utf8) ?? "")
    }

    /// The same thing, for a command slow enough that its silence is the
    /// problem. `run` hands back everything at once, which is right for a
    /// verify that takes a second and wrong for a 60 MB download: `onLine`
    /// fires as each line arrives, so the label under the spinner can say which
    /// step is running rather than "Working…" for a minute (#105).
    ///
    /// Blocking, like `run`, and on the caller's thread — never the main one.
    @discardableResult
    static func stream(_ path: String, _ args: [String],
                       onLine: @escaping (String) -> Void) -> Result {
        guard FileManager.default.isExecutableFile(atPath: path) else {
            return Result(status: 127, output: "\(path): not executable")
        }
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: path)
        proc.arguments = args
        let pipe = Pipe()
        proc.standardOutput = pipe
        proc.standardError = pipe
        do { try proc.run() } catch {
            return Result(status: 127, output: "\(path): \(error.localizedDescription)")
        }
        // Read as it comes, for the same reason `run` reads before waiting: a
        // child that fills the pipe while nobody drains it stops there. The
        // whole output is kept too, because a failure is explained by what was
        // said last, not by the exit code alone.
        var output = ""
        var pending = ""
        let handle = pipe.fileHandleForReading
        while case let chunk = handle.availableData, !chunk.isEmpty {
            let text = String(data: chunk, encoding: .utf8) ?? ""
            output += text
            pending += text
            // A read can end mid-line, so the tail is carried to the next one
            // rather than reported as a line of its own.
            let parts = pending.components(separatedBy: "\n")
            pending = parts.last ?? ""
            for line in parts.dropLast() where !line.isEmpty { onLine(line) }
        }
        if !pending.isEmpty { onLine(pending) }
        proc.waitUntilExit()
        return Result(status: proc.terminationStatus, output: output)
    }

    static func isRunning(_ pattern: String) -> Bool {
        run("/usr/bin/pgrep", ["-f", pattern]).ok
    }
}
