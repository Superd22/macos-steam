import Foundation

/// Running a command and keeping what it said.
///
/// The launcher shells out for exactly three things, all of them owned by
/// somebody else: `deploy.sh` (verify, uninstall, rollback — ADR 0010 says the
/// deploy module owns those, so reimplementing them here would be a second
/// implementation that can disagree), `cxbottle` (CodeWeavers' bottle creation,
/// whose incantation this app exists to keep out of the README), and `pgrep`.
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

    static func isRunning(_ pattern: String) -> Bool {
        run("/usr/bin/pgrep", ["-f", pattern]).ok
    }
}
