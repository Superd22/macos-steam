/* overlayinject — put Valve's overlay renderer into a title at process creation.
 *
 * ADR 0003 / #25. The renderer arms itself exactly once, when it loads, and the
 * deadline is NSApplication. Inside a Wine process that deadline is generous but
 * absolute: winemac.so (and with it NSApp) is demand-loaded on the process's
 * FIRST USER CALL, not with user32 — measured in attic/overlay-probe/u32probe.c.
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
 * the driver. instruments/overlay-probe/d3dprobe.c hit exactly that — as a console exe
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
 * payload — see src/shim/shim_pe.c. */
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

/* ---- import-table injection: be a static import, not a gatecrasher -------
 *
 * CREATE_SUSPENDED does NOT put a remote thread ahead of the title's own DLLs
 * under Wine, the way it does on Windows. The process is created with only ntdll
 * mapped and the main thread suspended, but the loader init (LdrInitializeThunk)
 * runs on whichever thread runs FIRST — and that is our injected thread. So
 * LoadLibraryW only returns after the title's entire static-import graph has
 * loaded and run its DllMains. Measured on Surviving Mars: our payload's DllMain
 * reports "user32=LOADED dxgi=LOADED", and by then dxgi has brought up
 * winemac.so and NSApplication, which is the deadline. Among Us survives it only
 * because its imports never touch USER — i.e. by luck, per title.
 *
 * So stop racing the loader and join it. In the suspended process, before
 * anything has read them, rewrite the exe's import directory so our payload is
 * the title's FIRST static import. The loader then initialises us before
 * user32, before dxgi, before anything can touch the display — an ordering it
 * guarantees rather than a head start we hope is big enough.
 *
 * The original directory has no spare room, so we build a new one in memory we
 * allocate inside the target and repoint the data directory at it. Everything
 * the loader reads is addressed as a 32-bit RVA from the image base, which is
 * why the allocation has to land within 2GB of the image (near_alloc).
 *
 * The injector's bitness always matches the target's (relaunch_for_bitness), so
 * the native IMAGE_NT_HEADERS is the right shape here — no cross-bitness parsing.
 */

typedef LONG (WINAPI *pNtQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG);

/* The import NAME the loader will resolve. A full path is deliberate: our
 * payload lives in C:\shim, which is on Wine's DLL path for finding the unixlib
 * beside the PE but is NOT on the loader's search path for a native import. The
 * basename must stay steamclient{,64}.dll regardless — ntdll derives the .so
 * name from the PE's basename, and that .so is what carries the constructor. */
#define IMPORT_FN "CreateInterface"        /* any real export of the payload */

static void *read_image_base(HANDLE proc)
{
    void *fn = (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    pNtQueryInformationProcess q = (pNtQueryInformationProcess)fn;
    struct { PVOID res1; PVOID peb; PVOID res2[2]; ULONG_PTR pid; ULONG_PTR res3; } pbi;
    void *base = NULL;
    SIZE_T got = 0;
    if (!q || q(proc, 0 /*ProcessBasicInformation*/, &pbi, sizeof(pbi), NULL)) return NULL;
    /* PEB->ImageBaseAddress: 0x10 on win64, 0x08 on win32. */
    if (!ReadProcessMemory(proc, (char *)pbi.peb + (sizeof(void *) == 8 ? 0x10 : 0x08),
                           &base, sizeof(base), &got) || got != sizeof(base))
        return NULL;
    return base;
}

/* RVAs are 32-bit, so our blob must sit within 2GB above the image. */
static void *near_alloc(HANDLE proc, void *image, SIZE_T size)
{
    char *p;
    for (p = (char *)image; p < (char *)image + 0x60000000; p += 0x10000) {
        void *got = VirtualAllocEx(proc, p, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (got) return got;
    }
    return NULL;
}

static int patch_imports(HANDLE proc)
{
    char hdr[0x1000];
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_IMPORT_DESCRIPTOR *blob = NULL;
    void *image, *remote;
    const char *name = sizeof(void *) == 8 ? "C:\\shim\\steamclient64.dll"
                                           : "C:\\shim\\steamclient.dll";
    SIZE_T got = 0, n = 0, descs, blob_size, off_int, off_iat, off_ibn, off_name;
    DWORD rva, oldprot;
    int ok = -1;

    image = read_image_base(proc);
    if (!image) { ilog("  patch: cannot read image base"); return -1; }
    if (!ReadProcessMemory(proc, image, hdr, sizeof(hdr), &got) || got != sizeof(hdr)) {
        ilog("  patch: cannot read headers"); return -1;
    }
    dos = (IMAGE_DOS_HEADER *)hdr;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { ilog("  patch: not MZ"); return -1; }
    nt = (IMAGE_NT_HEADERS *)(hdr + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { ilog("  patch: not PE"); return -1; }
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) { ilog("  patch: title has no import directory"); return -1; }

    /* Count the title's existing imports so we can copy them across. */
    {
        IMAGE_IMPORT_DESCRIPTOR d;
        for (n = 0; n < 4096; n++) {
            if (!ReadProcessMemory(proc, (char *)image + dir->VirtualAddress + n * sizeof(d),
                                   &d, sizeof(d), &got) || got != sizeof(d)) {
                ilog("  patch: cannot read import descriptor %lu", (unsigned long)n);
                return -1;
            }
            if (!d.Name && !d.FirstThunk) break;
        }
    }

    descs   = (n + 2) * sizeof(IMAGE_IMPORT_DESCRIPTOR);   /* ours + theirs + null */
    off_int = descs;
    off_iat = off_int + 2 * sizeof(ULONG_PTR);
    off_ibn = off_iat + 2 * sizeof(ULONG_PTR);
    off_name = off_ibn + sizeof(WORD) + sizeof(IMPORT_FN);
    blob_size = off_name + lstrlenA(name) + 1;

    remote = near_alloc(proc, image, blob_size);
    if (!remote) { ilog("  patch: no free page within 2GB of the image"); return -1; }
    rva = (DWORD)((char *)remote - (char *)image);

    blob = (IMAGE_IMPORT_DESCRIPTOR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, blob_size);
    if (!blob) { ilog("  patch: out of memory"); return -1; }

    /* [0] is OURS — first, so the loader initialises us before the title's own
     * imports. [1..n] are the title's, copied verbatim. [n+1] stays zero. */
    blob[0].OriginalFirstThunk = rva + (DWORD)off_int;
    blob[0].FirstThunk         = rva + (DWORD)off_iat;
    blob[0].Name               = rva + (DWORD)off_name;
    if (n && (!ReadProcessMemory(proc, (char *)image + dir->VirtualAddress, &blob[1],
                                 n * sizeof(IMAGE_IMPORT_DESCRIPTOR), &got)
              || got != n * sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
        ilog("  patch: cannot copy the title's imports");
        HeapFree(GetProcessHeap(), 0, blob);
        return -1;
    }

    {
        char *b = (char *)blob;
        ULONG_PTR thunk = rva + (DWORD)off_ibn;          /* -> IMAGE_IMPORT_BY_NAME */
        memcpy(b + off_int, &thunk, sizeof(thunk));      /* INT: one entry, then null */
        memcpy(b + off_iat, &thunk, sizeof(thunk));      /* IAT: same, loader overwrites */
        *(WORD *)(b + off_ibn) = 0;                      /* hint */
        memcpy(b + off_ibn + sizeof(WORD), IMPORT_FN, sizeof(IMPORT_FN));
        memcpy(b + off_name, name, lstrlenA(name) + 1);
    }

    if (!WriteProcessMemory(proc, remote, blob, blob_size, &got) || got != blob_size) {
        ilog("  patch: cannot write the new import table"); goto out;
    }

    /* Repoint the data directory at our table. The headers are read-only, and a
     * bound-import directory would let the loader skip resolution entirely, so
     * clear that too while we are in here. */
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = rva;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = (DWORD)descs;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT].VirtualAddress = 0;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT].Size = 0;

    if (!VirtualProtectEx(proc, image, sizeof(hdr), PAGE_READWRITE, &oldprot)) {
        ilog("  patch: cannot unprotect headers (%lu)", GetLastError()); goto out;
    }
    ok = (WriteProcessMemory(proc, image, hdr, sizeof(hdr), &got) && got == sizeof(hdr)) ? 0 : -1;
    VirtualProtectEx(proc, image, sizeof(hdr), oldprot, &oldprot);
    if (ok) { ilog("  patch: cannot write headers back"); goto out; }

    ilog("  patched imports: %s is now import [0] of %lu (image=%p table rva=0x%lx)",
         name, (unsigned long)(n + 1), image, (unsigned long)rva);
out:
    HeapFree(GetProcessHeap(), 0, blob);
    return ok;
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

    /* Truncation here would hand the sibling a different command line than the
     * one we were given, so treat a short write as a failure rather than
     * launching whatever fitted. */
    if (_snprintf(line, sizeof(line), "\"%s\" %s", sib, cmdline) < 0) {
        ilog("  command line too long to hand over to %s", sib);
        return -1;
    }
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
                /* A child is stopped here before its own loader init, so the
                 * same import patch applies and is preferred for the same
                 * reason. inject(.., 0) does not wait: under a debugger the
                 * remote thread's own LOAD_DLL events cannot be drained until
                 * we return to this loop. */
                if (child) {
                    if (patch_imports(child)) inject(child, 0);
                    CloseHandle(child);
                }
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

/* ---- command line assembly ----------------------------------------------
 *
 * lstrcatA has no bound, and the arguments here are not ours: Steam hands us the
 * title's path plus whatever the user typed into its launch options. A long
 * enough one used to run off the end of a fixed 8 KB stack buffer, and an
 * argument containing a double quote used to close our quoting early and inject
 * extra tokens into the command line CreateProcess parses.
 *
 * So append one argument at a time, bounded, and quote it the way
 * CommandLineToArgvW unquotes it: backslash runs are doubled only where they
 * precede a quote (or the closing quote), and an embedded quote is escaped.
 * Returns -1 when the argument would not fit — the caller must then refuse to
 * launch, because a truncated command line is a wrong launch, not a smaller one.
 */
static int append_arg(char *dst, size_t cap, const char *arg)
{
    size_t n0 = strlen(dst), n = n0, bs = 0;
    const char *p;

    if (n && n + 1 >= cap) goto full;
    if (n) dst[n++] = ' ';
    if (n + 2 >= cap) goto full;                 /* opening quote + NUL */
    dst[n++] = '"';

    for (p = arg; *p; p++) {
        if (*p == '\\') { bs++; continue; }
        if (*p == '"') {
            if (n + bs * 2 + 2 + 2 > cap) goto full;   /* + closing quote + NUL */
            for (; bs; bs--) { dst[n++] = '\\'; dst[n++] = '\\'; }
            dst[n++] = '\\'; dst[n++] = '"';
        } else {
            if (n + bs + 1 + 2 > cap) goto full;
            for (; bs; bs--) dst[n++] = '\\';
            dst[n++] = *p;
        }
    }
    /* Trailing backslashes would otherwise escape the closing quote. */
    if (n + bs * 2 + 2 > cap) goto full;
    for (; bs; bs--) { dst[n++] = '\\'; dst[n++] = '\\'; }
    dst[n++] = '"';
    dst[n] = 0;
    return 0;

full:
    dst[n0] = 0;                                 /* leave the caller's buffer intact */
    return -1;
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

    if (argc < 2) { ilog("usage: overlayinject <title.exe> [args...]\n"
                         "       overlayinject --attach <pid>"); return 2; }

    /* --attach <pid>: patch a SUSPENDED process somebody else created (#27).
     *
     * This exists because DEBUG_PROCESS cannot cover the case that matters. A
     * title like Space Marine ships a 32-bit bootstrapper that starts the real
     * 64-bit game, and a 32-bit debugger cannot debug a 64-bit child — on
     * Windows or under Wine. Measured: no CREATE_PROCESS_DEBUG_EVENT ever
     * arrives, so the child ran with no payload and the overlay never armed.
     *
     * So the parent's own CreateProcess is hooked instead (shim_pe.c), and it
     * calls US, in the CHILD's bitness, to do the patch it cannot do itself.
     * We only patch: the hook owns the resume, because it is the one that knows
     * whether its caller asked for CREATE_SUSPENDED. */
    if (!strcmp(argv[1], "--attach")) {
        DWORD pid = argc > 2 ? (DWORD)strtoul(argv[2], NULL, 10) : 0;
        HANDLE h;
        int rc;
        if (!pid) { ilog("--attach: no pid"); return 2; }
        h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        if (!h) { ilog("--attach %lu: OpenProcess failed %lu", pid, GetLastError()); return 2; }
        ilog("--- overlayinject (%d-bit) --attach pid=%lu",
             (int)sizeof(void *) * 8, pid);
        rc = patch_imports(h);
        if (rc == 0) ilog("  attached: payload is the child's first static import");
        else if (inject(h, 1) == 0) ilog("  attached: FELL BACK to remote thread");
        else { ilog("  attach FAILED — child runs without an overlay"); rc = 2; }
        CloseHandle(h);
        return rc == 0 ? 0 : 2;
    }

    /* Rebuild a command line for the title from argv[1..], quoted so paths with
     * spaces survive — Steam library paths routinely have them — and bounded,
     * because none of these arguments are ours (see append_arg). */
    for (i = 1; i < argc; i++) {
        if (append_arg(line, sizeof line, argv[i])) {
            ilog("command line exceeds %u bytes at argv[%d] — refusing to launch"
                 " a truncated one", (unsigned)sizeof line, i);
            return 2;
        }
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

    /* Import-table injection first: it is the only method with a guaranteed
     * ordering (we become the title's first static import). The remote-thread
     * path is kept as a fallback because it still wins on titles whose own
     * imports never touch USER — better a title-dependent overlay than none. */
    if (patch_imports(pi.hProcess) == 0) {
        ilog("payload will load as a static import (guaranteed before the title's own)");
    } else if (inject(pi.hProcess, 1) == 0) {
        ilog("FELL BACK to remote-thread injection — the overlay may lose the race on"
             " titles whose static imports touch USER (see #25)");
    } else {
        ilog("INJECTION FAILED — the title will run, without an overlay");
    }

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
