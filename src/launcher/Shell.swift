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
///
/// Every one of them is somebody else's process, which is why the timeouts here
/// are real and not decoration: until #108 `run` took a `timeout` and never
/// referenced it, so `Preflight.createBottle`'s `timeout: 300` bounded nothing
/// and a stalled `cxbottle` was an app that had stopped responding with no way
/// back.
enum Shell {
    struct Result {
        let status: Int32
        let output: String
        /// We killed it; it did not choose this exit code. A caller has to be
        /// able to tell the two apart, because "the command said no" and "the
        /// command never answered" are different sentences to put in front of a
        /// user. Stored rather than a sentinel status: 124 below is `timeout(1)`'s
        /// convention and good enough to print, but a child is free to exit 124
        /// on its own and this flag is not.
        var timedOut: Bool = false
        var ok: Bool { status == 0 }
    }

    /// What a killed child's status reads as. `timeout(1)`'s number, so a status
    /// that reaches a log or a Diagnose report is not mistaken for a clean exit.
    static let timeoutStatus: Int32 = 124

    /// Blocking, on the caller's thread — never the main one.
    ///
    /// `timeout` is the whole operation: spawn, output, exit. A child still
    /// alive at the deadline is killed and reported as `timedOut`, with whatever
    /// it managed to say kept — a stalled command's last line is usually the
    /// only clue about where it stalled.
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
        // line per problem. Which is also why the bound cannot be a timer around
        // `readDataToEndOfFile` — the read has to keep happening while the clock
        // runs, so the wait IS the read (see `drain`).
        let deadline = Date().addingTimeInterval(timeout)
        var data = Data()
        let finished = drain(pipe.fileHandleForReading, until: deadline, idle: nil) { data.append($0) }
            && wait(proc, until: deadline)
        return finish(proc, output: String(data: data, encoding: .utf8) ?? "", finished: finished)
    }

    /// The same thing, for a command slow enough that its silence is the
    /// problem. `run` hands back everything at once, which is right for a
    /// verify that takes a second and wrong for a 60 MB download: `onLine`
    /// fires as each line arrives, so the label under the spinner can say which
    /// step is running rather than "Working…" for a minute (#105).
    ///
    /// Blocking, like `run`, and on the caller's thread — never the main one.
    ///
    /// `idleTimeout` is deliberately not the same clock as `run`'s: these are the
    /// commands with no honest total bound — 60 MB over an unknown line takes as
    /// long as it takes, and a wall-clock cap would kill a download that was
    /// working. What is never legitimate is a command that narrates its steps
    /// going quiet, so the budget is time *between* lines and it resets on every
    /// one (#108). `fetch.sh`'s `curl --max-time` still holds; this is the bound
    /// for the day a caller arrives without one of its own.
    @discardableResult
    static func stream(_ path: String, _ args: [String], idleTimeout: TimeInterval = 120,
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
        let quiet = drain(pipe.fileHandleForReading, until: .distantFuture, idle: idleTimeout) { chunk in
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
        // A child that closed its pipe and then hung is still a hang, and the
        // same silence budget is the right one to give it to finish.
        let finished = quiet && wait(proc, until: Date().addingTimeInterval(idleTimeout))
        return finish(proc, output: output, finished: finished)
    }

    static func isRunning(_ pattern: String) -> Bool {
        run("/usr/bin/pgrep", ["-f", pattern]).ok
    }

    // MARK: - the bound

    /// Read a child's pipe until it closes it, or until the clock runs out.
    ///
    /// This is the whole trick of #108. Draining and waiting cannot be two
    /// phases — the read blocks before any wait is reached, and stopping the
    /// read to watch a clock is the pipe-buffer deadlock the comment in `run`
    /// warns about. So the clock is inside the read: `poll(2)` sleeps on the
    /// pipe for exactly what is left of the budget and returns the instant a
    /// byte arrives, which costs a fast command nothing.
    ///
    /// `idle` (`stream`) restarts the budget on every chunk; `nil` (`run`) makes
    /// `until` a hard wall. Returns false when the clock won.
    private static func drain(_ handle: FileHandle, until hard: Date, idle: TimeInterval?,
                              onChunk: (Data) -> Void) -> Bool {
        let fd = handle.fileDescriptor
        var quiet = idle.map { Date().addingTimeInterval($0) } ?? .distantFuture
        var buffer = [UInt8](repeating: 0, count: 65536)
        while true {
            let deadline = min(hard, quiet)
            let left = deadline.timeIntervalSinceNow
            if left <= 0 { return false }
            var pfd = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
            // Milliseconds, and capped: `poll` takes an Int32 and a budget of
            // `.distantFuture` overflows it. An hour is not a bound anyone
            // relies on — it is just a number that fits, and the loop re-arms.
            let n = poll(&pfd, 1, Int32(min(left * 1000, 3_600_000).rounded(.up)))
            if n == 0 { return false }
            if n < 0 {
                if errno == EINTR { continue }
                // Nothing left to read from and no way to ask why. Treat it as
                // the end of the output rather than as a timeout: the exit
                // status is still the honest answer.
                return true
            }
            let count = read(fd, &buffer, buffer.count)
            if count < 0 {
                if errno == EINTR || errno == EAGAIN { continue }
                return true
            }
            if count == 0 { return true }  // the child closed the pipe
            onChunk(Data(buffer[0..<count]))
            if let idle { quiet = Date().addingTimeInterval(idle) }
        }
    }

    /// EOF on the pipe is not the same event as exit — a child can close its
    /// output and keep running — so the wait gets a bound of its own. Polled
    /// rather than `waitUntilExit`, which is the other unbounded call #108 is
    /// about. The sleep is short because this loop is on the path of every
    /// `pgrep`: by the time the pipe is closed the child is already gone, and
    /// what is being waited on is only `Process` noticing.
    /// Measured over 20 `/bin/echo` runs: 67 ms a call before this change, 72 ms
    /// with a 1 ms sleep, 77 ms with a 5 ms one — so the sleep is the granularity
    /// of the whole change, against the ~67 ms `Process` spends spawning anything
    /// at all.
    private static func wait(_ proc: Process, until deadline: Date) -> Bool {
        while proc.isRunning {
            if Date() >= deadline { return false }
            Thread.sleep(forTimeInterval: 0.001)
        }
        proc.waitUntilExit()
        return true
    }

    /// Common ending: a child that ran out of clock is killed here rather than
    /// left behind. Orphaning it is not a fix — a runaway `cxbottle` still holds
    /// the bottle it was half-way through creating, and a runaway `curl` still
    /// spends the user's 60 MB.
    private static func finish(_ proc: Process, output: String, finished: Bool) -> Result {
        guard !finished else {
            return Result(status: proc.terminationStatus, output: output)
        }
        stop(proc)
        return Result(status: timeoutStatus, output: output, timedOut: true)
    }

    /// SIGTERM, then SIGKILL for whatever ignores it. Only the direct child:
    /// `Process` does not give it a process group of its own, so it shares ours
    /// and killing the group would kill the launcher. A shell wrapper that
    /// spawned helpers can therefore leave some behind — everything we run
    /// either is the work (`cxbottle`, `pgrep`, `osascript`) or is a script
    /// whose helpers die with the pipe.
    private static func stop(_ proc: Process) {
        guard proc.isRunning else { proc.waitUntilExit(); return }
        proc.terminate()
        if wait(proc, until: Date().addingTimeInterval(2)) { return }
        kill(proc.processIdentifier, SIGKILL)
        _ = wait(proc, until: Date().addingTimeInterval(2))
    }
}
