/* The DRM route's hook (ADR 0014).
 *
 * Steam's DRM stub verifies an RSA signature over whichever file provides the
 * CreateInterface it just resolved. Our shim cannot carry one — a PE is either
 * a Wine builtin or Valve-signed, because winebuild's marker and Valve's "VLV"
 * block are the same bytes at offset 0x40. So the file at that path is VALVE'S,
 * genuine and untouched, and the wrapper's question is answered honestly.
 *
 * This library is how our code gets in front of it. Valve's steamclient64.dll
 * imports two non-system libraries of its own; the wrapper loads it with
 * LOAD_WITH_ALTERED_SEARCH_PATH, so those resolve from ITS directory first —
 * a directory we lay down. The loader therefore initialises us before its entry
 * point runs and long before the wrapper asks for CreateInterface, and by then
 * its image is mapped. We:
 *
 *   - rewrite each of its exports to jump into our shim, where we have one
 *   - point the rest at a named stub that answers zero and says so
 *   - overwrite its entry point with `mov eax,1 ; ret`
 *
 * so no Valve client code executes in the process. That is what Proton does in
 * its own ntdll (steamclient_setup_trampolines + LDR_DONT_CALL_DLLMAIN); we do
 * it from the search path instead, because we do not ship a Wine — and because
 * the alternative, injecting into a suspended title, is what anti-tamper
 * titles reject (ADR 0003).
 *
 * The forwarders bind LAZILY. Calling LoadLibrary from DllMain runs under the
 * loader lock, and the shim is a builtin whose unix half has its own load path;
 * resolving on first call keeps that out of the lock entirely.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "shim_paths.h"

/* ---- logging ------------------------------------------------------------
 * OutputDebugStringA always, so a launch traced with +debugstr shows the
 * install without any extra setup, the way the shim's own PE half does. */
static void dbg(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}

/* ---- forwarders ---------------------------------------------------------
 * One per name our shim exports. Signatures are the Steamworks ones, so the
 * jump is ABI-identical and no argument is touched on the way through.
 *
 * When the shim grows exports (#98), this table grows with it. It is not
 * generated, because a wrong signature here is silent, and the install below
 * NAMES every export it could not map so the gap is visible rather than
 * assumed away. */
static HMODULE shim;

static void *shim_proc(const char *name)
{
    void *p;
    if (!shim)
    {
        shim = LoadLibraryA(SHIM_PATH_LSTEAM_PE64_WIN);
        if (!shim) dbg("drm: LoadLibrary(%s) FAILED, err %lu -- the title will "
                       "get zero from every Steamworks entry point",
                       SHIM_PATH_LSTEAM_PE64_WIN, GetLastError());
    }
    p = shim ? (void *)GetProcAddress(shim, name) : NULL;
    if (!p) dbg("drm: %s missing from the shim", name);
    return p;
}

#define FORWARD(ret, name, params, args, fail)                                 \
    static ret (*p_##name) params;                                             \
    static ret fwd_##name params {                                             \
        if (!p_##name) p_##name = (ret (*) params)shim_proc(#name);            \
        if (!p_##name) return fail;                                            \
        return p_##name args;                                                  \
    }

FORWARD(void *, CreateInterface, (const char *n, int *rc), (n, rc), NULL)
FORWARD(int, Steam_BGetCallback, (int32_t p, void *m), (p, m), 0)
FORWARD(int, Steam_GetAPICallResult,
        (int32_t p, uint64_t c, void *b, int cb, int e, void *f), (p, c, b, cb, e, f), 0)
FORWARD(int, Steam_IsKnownInterface, (const char *v), (v), 1)

/* void-returning ones cannot use the macro's `return fail` */
static void (*p_Steam_FreeLastCallback)(int32_t);
static void fwd_Steam_FreeLastCallback(int32_t pipe)
{
    if (!p_Steam_FreeLastCallback)
        p_Steam_FreeLastCallback = (void (*)(int32_t))shim_proc("Steam_FreeLastCallback");
    if (p_Steam_FreeLastCallback) p_Steam_FreeLastCallback(pipe);
}

static void (*p_Steam_ReleaseThreadLocalMemory)(int);
static void fwd_Steam_ReleaseThreadLocalMemory(int flag)
{
    if (!p_Steam_ReleaseThreadLocalMemory)
        p_Steam_ReleaseThreadLocalMemory = (void (*)(int))shim_proc("Steam_ReleaseThreadLocalMemory");
    if (p_Steam_ReleaseThreadLocalMemory) p_Steam_ReleaseThreadLocalMemory(flag);
}

static void (*p_Steam_NotifyMissingInterface)(int32_t, const char *);
static void fwd_Steam_NotifyMissingInterface(int32_t pipe, const char *ver)
{
    if (!p_Steam_NotifyMissingInterface)
        p_Steam_NotifyMissingInterface =
            (void (*)(int32_t, const char *))shim_proc("Steam_NotifyMissingInterface");
    if (p_Steam_NotifyMissingInterface) p_Steam_NotifyMissingInterface(pipe, ver);
}

static const struct { const char *name; void *fn; } FORWARDS[] = {
    { "CreateInterface",                (void *)fwd_CreateInterface },
    { "Steam_BGetCallback",             (void *)fwd_Steam_BGetCallback },
    { "Steam_FreeLastCallback",         (void *)fwd_Steam_FreeLastCallback },
    { "Steam_GetAPICallResult",         (void *)fwd_Steam_GetAPICallResult },
    { "Steam_ReleaseThreadLocalMemory", (void *)fwd_Steam_ReleaseThreadLocalMemory },
    { "Steam_IsKnownInterface",         (void *)fwd_Steam_IsKnownInterface },
    { "Steam_NotifyMissingInterface",   (void *)fwd_Steam_NotifyMissingInterface },
};

/* ---- unmapped exports ---------------------------------------------------
 * An export we cannot serve answers zero. That is a wrong answer, and #45's
 * whole argument is that a wrong answer given quietly costs a session to find —
 * so each one gets its own thunk carrying its name, and says so the first time
 * it is called. Valve's code is never the fallback: it does not run. */
static uint64_t dead_called(const char *name)
{
    dbg("drm: UNSERVED export %s called -- answering 0 (the shim does not "
        "export it; see #98)", name);
    return 0;
}

/* The fallback when no thunk could be allocated. It must NOT be dead_called:
 * that reads its first argument as a name, and reached directly it would get
 * whatever the title passed and format it with %s. */
static uint64_t dead_unnamed(void)
{
    dbg("drm: an unserved export was called -- answering 0 (no thunk pool; the "
        "install log above names every unserved export)");
    return 0;
}

static unsigned char *dead_pool;
static SIZE_T dead_used, dead_size;

/* mov rcx, <name> ; mov rax, dead_called ; jmp rax
 * Clobbering rcx is deliberate: it is the first argument, and we are about to
 * discard every argument anyway. */
static void *dead_thunk(const char *name)
{
    unsigned char *t;
    void *fn = (void *)dead_called;
    if (dead_used + 22 > dead_size) return (void *)dead_unnamed;
    t = dead_pool + dead_used;
    dead_used += 22;
    t[0] = 0x48; t[1] = 0xB9; memcpy(t + 2,  &name, 8);   /* mov rcx, name */
    t[10] = 0x48; t[11] = 0xB8; memcpy(t + 12, &fn, 8);   /* mov rax, fn   */
    t[20] = 0xFF; t[21] = 0xE0;                           /* jmp rax       */
    return t;
}

/* ---- the install --------------------------------------------------------- */

/* movabs rax, target ; jmp rax */
static int write_jump(void *at, void *target)
{
    unsigned char code[12];
    DWORD old;
    if (!VirtualProtect(at, sizeof code, PAGE_EXECUTE_READWRITE, &old)) return 0;
    code[0] = 0x48; code[1] = 0xB8;
    memcpy(code + 2, &target, 8);
    code[10] = 0xFF; code[11] = 0xE0;
    memcpy(at, code, sizeof code);
    VirtualProtect(at, sizeof code, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof code);
    return 1;
}

static void install(void)
{
    HMODULE sc = GetModuleHandleA(SHIM_PATH_VALVE_PE64);
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_EXPORT_DIRECTORY *ed;
    DWORD *fns, *nms, expva, expsz, i, served = 0, unserved = 0;
    WORD *ords;

    if (!sc)
    {
        /* We are loaded as somebody else's dependency, which is legitimate:
         * the 32-bit client, a helper, a title that ships its own tier0. Do
         * nothing rather than guess. */
        dbg("drm: %s not mapped -- no trampolines installed", SHIM_PATH_VALVE_PE64);
        return;
    }
    dos = (IMAGE_DOS_HEADER *)sc;
    nt  = (IMAGE_NT_HEADERS *)((char *)sc + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { dbg("drm: %s is not a PE", SHIM_PATH_VALVE_PE64); return; }
    expva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    expsz = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
    if (!expva) { dbg("drm: %s has no export directory", SHIM_PATH_VALVE_PE64); return; }

    ed   = (IMAGE_EXPORT_DIRECTORY *)((char *)sc + expva);
    fns  = (DWORD *)((char *)sc + ed->AddressOfFunctions);
    nms  = (DWORD *)((char *)sc + ed->AddressOfNames);
    ords = (WORD  *)((char *)sc + ed->AddressOfNameOrdinals);

    dead_size = ((SIZE_T)ed->NumberOfNames * 22 + 0xFFF) & ~(SIZE_T)0xFFF;
    dead_pool = VirtualAlloc(NULL, dead_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!dead_pool) dead_size = 0;

    for (i = 0; i < ed->NumberOfNames; i++)
    {
        const char *nm = (const char *)sc + nms[i];
        DWORD rva = fns[ords[i]];
        void *at = (char *)sc + rva, *tgt = NULL;
        unsigned j;

        /* A forwarder's RVA points inside the export directory at a string,
         * not at code. Rewriting it would corrupt the table. */
        if (rva >= expva && rva < expva + expsz) continue;

        for (j = 0; j < sizeof FORWARDS / sizeof *FORWARDS; j++)
            if (!strcmp(nm, FORWARDS[j].name)) { tgt = FORWARDS[j].fn; break; }

        if (tgt) served++;
        else { tgt = dead_thunk(nm); unserved++; }

        if (!write_jump(at, tgt))
            dbg("drm: could not rewrite %s (err %lu)", nm, GetLastError());
    }

    /* Valve's own code must never run: the trampolines are only half of it,
     * and its DllMain is what would go looking for a Windows Steam client. */
    {
        unsigned char ret_true[6] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
        void *ep = (char *)sc + nt->OptionalHeader.AddressOfEntryPoint;
        DWORD old;
        if (VirtualProtect(ep, sizeof ret_true, PAGE_EXECUTE_READWRITE, &old))
        {
            memcpy(ep, ret_true, sizeof ret_true);
            VirtualProtect(ep, sizeof ret_true, old, &old);
            FlushInstructionCache(GetCurrentProcess(), ep, sizeof ret_true);
        }
        else
        {
            /* Fatal in substance: its DllMain is about to run and will fail
             * looking for a client that is not there. Say so plainly. */
            dbg("drm: COULD NOT neuter %s's entry point (err %lu) -- Valve's "
                "client code is about to run and the launch will fail",
                SHIM_PATH_VALVE_PE64, GetLastError());
        }
    }

    dbg("drm: %s trampolined -- %lu exports to the shim, %lu unserved",
        SHIM_PATH_VALVE_PE64, served, unserved);
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, void *reserved)
{
    (void)self; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) install();
    return TRUE;
}
