/* overlayinject — put Valve's overlay renderer into a title at process creation.
 *
 * ADR 0003 / #25. The renderer arms itself exactly once, when it loads, and the
 * deadline is NSApplication. Inside a Wine process that deadline is generous but
 * absolute: winemac.so (and with it NSApp) is demand-loaded on the process's
 * FIRST USER CALL, not with user32 — measured in tools/overlay-probe/u32probe.c.
 * So anything running before the title's entry point wins, and anything after
 * its first window call has already lost.
 *
 * We create the title ourselves, so we do not need to be loaded by it:
 *
 *   CreateProcess(CREATE_SUSPENDED)   only ntdll mapped, main thread never ran,
 *                                     the title's static imports UNRESOLVED
 *   inject steamclient{,64}.dll       its DllMain binds the unixlib, whose
 *                                     constructor dlopens the renderer
 *   ResumeThread                      the title starts, already hooked
 *
 * Why suspended rather than the initial debug breakpoint: at the breakpoint every
 * static-import DllMain has already run, and one USER call in any of them loads
 * the driver. tools/overlay-probe/d3dprobe.c hit exactly that — as a console exe
 * it lost the race before main, because the console attach reaches USER.
 *
 * Child processes (a launcher exe, a 32-bit stub starting a 64-bit binary) render
 * in a process we never created, so after injecting we attach as a debugger and
 * inject into each child as it appears. SHIM_INJECT_CHILDREN=0 turns that off if
 * being a debuggee ever upsets a title; the main process is still covered.
 *
 * Usage:  overlayinject.exe <title.exe> [args...]
 * The title's exit code is propagated — Steam reads it through the compat tool.
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

static FILE *g_log;

static void ilog(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) {
        char p[MAX_PATH];
        if (!GetEnvironmentVariableA("SHIM_INJECT_LOG", p, sizeof(p)))
            lstrcpynA(p, "C:\\overlayinject.log", sizeof(p));
        g_log = fopen(p, "a");
        if (!g_log) return;
        setvbuf(g_log, NULL, _IONBF, 0);
    }
    va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
}

/* ---- the payload -------------------------------------------------------- */

/* The shim PE for OUR bitness, which is the target's (see relaunch_for_bitness).
 * Its DllMain binds the unixlib when SHIM_OVERLAY is set; that is the whole
 * payload — see tools/shim/shim_pe.c. */
static const wchar_t *payload_path(void)
{
    static wchar_t buf[MAX_PATH];
    if (GetEnvironmentVariableW(L"SHIM_OVERLAY_PAYLOAD", buf, MAX_PATH)) return buf;
    lstrcpynW(buf, sizeof(void *) == 8 ? L"C:\\shim\\steamclient64.dll"
                                       : L"C:\\shim\\steamclient.dll", MAX_PATH);
    return buf;
}

/* Write the payload's path into the target and run LoadLibraryW on it there.
 *
 * kernel32 is at the same address in every process of the same bitness, so our
 * own LoadLibraryW address is valid in the target — the standard trick, and the
 * reason relaunch_for_bitness exists.
 *
 * wait=1 for the main process: it is suspended and NOT yet a debuggee, so the
 * remote thread runs freely and we can confirm the DLL loaded before resuming.
 * wait=0 for children: they ARE debuggees, stopped at a debug event, so waiting
 * here would deadlock — the remote thread's own LOAD_DLL events cannot be
 * drained until we return to the debug loop. Fire and continue; the loop drains.
 */
static int inject(HANDLE proc, int wait)
{
    const wchar_t *dll = payload_path();
    SIZE_T bytes = (lstrlenW(dll) + 1) * sizeof(wchar_t);
    void *remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HANDLE th;
    DWORD code = 0;

    if (!remote) { ilog("  VirtualAllocEx failed %lu", GetLastError()); return -1; }
    if (!WriteProcessMemory(proc, remote, dll, bytes, NULL)) {
        ilog("  WriteProcessMemory failed %lu", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return -1;
    }
    /* The cast goes through a void* on purpose: FARPROC and
     * LPTHREAD_START_ROUTINE differ in signature, and a direct cast is a
     * -Wcast-function-type warning rather than a real problem. */
    {
        void *loadlib = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryW");
        th = CreateRemoteThread(proc, NULL, 0, (LPTHREAD_START_ROUTINE)loadlib, remote, 0, NULL);
    }
    if (!th) {
        ilog("  CreateRemoteThread failed %lu", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return -1;
    }
    if (wait) {
        WaitForSingleObject(th, 15000);
        GetExitCodeThread(th, &code);          /* the HMODULE, truncated: 0 = failed */
        ilog("  injected %ls -> LoadLibraryW returned %s",
             dll, code ? "a module" : "NULL (FAILED)");
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    } else {
        ilog("  injected %ls (async, child)", dll);
        /* remote stays allocated: one page, freed with the child's address space */
    }
    CloseHandle(th);
    return wait && !code ? -1 : 0;
}

/* ---- bitness ------------------------------------------------------------- */

/* CreateRemoteThread across a bitness boundary is not a thing: our LoadLibraryW
 * address would be meaningless in the target. Read the title's PE header and, if
 * it disagrees with us, hand the whole job to our sibling build. */
static int target_is_64bit(const char *exe, int *ok)
{
    HANDLE f = CreateFileA(exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD got = 0, pe = 0;
    WORD machine = 0;
    *ok = 0;
    if (f == INVALID_HANDLE_VALUE) return 0;
    SetFilePointer(f, 0x3c, NULL, FILE_BEGIN);
    if (ReadFile(f, &pe, sizeof(pe), &got, NULL) && got == sizeof(pe)) {
        SetFilePointer(f, pe + 4, NULL, FILE_BEGIN);       /* skip "PE\0\0" */
        if (ReadFile(f, &machine, sizeof(machine), &got, NULL) && got == sizeof(machine))
            *ok = 1;
    }
    CloseHandle(f);
    return machine == IMAGE_FILE_MACHINE_AMD64;
}

static int relaunch_for_bitness(int want64, char *cmdline)
{
    char self[MAX_PATH], sib[MAX_PATH], line[8192];
    char *slash;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;

    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);

    GetModuleFileNameA(NULL, self, MAX_PATH);
    lstrcpynA(sib, self, MAX_PATH);
    slash = strrchr(sib, '\\');
    lstrcpyA(slash ? slash + 1 : sib, want64 ? "overlayinject64.exe" : "overlayinject32.exe");
    ilog("target is %d-bit, we are %d-bit -> handing over to %s",
         want64 ? 64 : 32, (int)sizeof(void *) * 8, sib);

    _snprintf(line, sizeof(line), "\"%s\" %s", sib, cmdline);
    line[sizeof(line) - 1] = 0;
    if (!CreateProcessA(sib, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        ilog("  cannot start %s (%lu)", sib, GetLastError());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return (int)code;
}

/* ---- debug loop: cover processes the title starts ------------------------ */

static DWORD debug_loop(DWORD top_pid, HANDLE top_proc)
{
    DEBUG_EVENT ev;
    DWORD exit_code = 0;

    for (;;) {
        if (!WaitForDebugEvent(&ev, INFINITE)) break;

        if (ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            if (ev.dwProcessId != top_pid) {
                HANDLE child = OpenProcess(PROCESS_ALL_ACCESS, FALSE, ev.dwProcessId);
                ilog("child process %lu started", ev.dwProcessId);
                if (child) { inject(child, 0); CloseHandle(child); }
                else ilog("  OpenProcess failed %lu", GetLastError());
            }
            if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
        } else if (ev.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
            if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
        } else if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            if (ev.dwProcessId == top_pid) {
                exit_code = ev.u.ExitProcess.dwExitCode;
                ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
                break;
            }
        }

        /* Exceptions are the title's business, not ours: hand every one back
         * unhandled so its own handlers (and its crash reporter) still see it.
         * Swallowing these would turn us into a silent bug-hider. */
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId,
                           ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT
                               ? DBG_EXCEPTION_NOT_HANDLED : DBG_CONTINUE);
    }
    if (!exit_code) GetExitCodeProcess(top_proc, &exit_code);
    return exit_code;
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char line[8192] = {0};
    char childflag[8] = {0};
    char *raw;
    int i, is64, readable, children;
    DWORD exit_code = 0;

    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);

    if (argc < 2) { ilog("usage: overlayinject <title.exe> [args...]"); return 2; }

    /* Rebuild a command line for the title from argv[1..], quoted so paths with
     * spaces survive — Steam library paths routinely have them. */
    for (i = 1; i < argc; i++) {
        lstrcatA(line, i > 1 ? " \"" : "\"");
        lstrcatA(line, argv[i]);
        lstrcatA(line, "\"");
    }
    ilog("--- overlayinject (%d-bit) %s", (int)sizeof(void *) * 8, line);

    is64 = target_is_64bit(argv[1], &readable);
    if (!readable)
        ilog("cannot read %s's PE header — assuming our own bitness", argv[1]);
    else if (is64 != (sizeof(void *) == 8)) {
        /* Hand over everything after our own exe name, unchanged. */
        raw = GetCommandLineA();
        raw = raw[0] == '"' ? strchr(raw + 1, '"') : strchr(raw, ' ');
        return relaunch_for_bitness(is64, raw ? raw + 1 : "");
    }

    if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_SUSPENDED,
                        NULL, NULL, &si, &pi)) {
        ilog("CreateProcess failed %lu — the title did not start", GetLastError());
        return 2;
    }
    ilog("created pid=%lu suspended", pi.dwProcessId);

    if (inject(pi.hProcess, 1))
        ilog("INJECTION FAILED — the title will run, without an overlay");

    children = 1;                                  /* default on: cover relaunchers */
    if (GetEnvironmentVariableA("SHIM_INJECT_CHILDREN", childflag, sizeof(childflag)) > 0)
        children = childflag[0] != '0';
    if (children && !DebugActiveProcess(pi.dwProcessId)) {
        ilog("DebugActiveProcess failed %lu — child processes will not be covered",
             GetLastError());
        children = 0;
    }
    /* Our death would kill the title with us; we outlive it either way, but say
     * so explicitly rather than relying on it. */
    if (children) DebugSetProcessKillOnExit(FALSE);

    ResumeThread(pi.hThread);
    ilog("resumed (children=%s)", children ? "debugger" : "off");

    if (children) exit_code = debug_loop(pi.dwProcessId, pi.hProcess);
    else {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &exit_code);
    }

    ilog("title exited, code=%lu", exit_code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return (int)exit_code;
}
