/* shim_pe.c — the PE half of the achievement shim (#11): steamclient64.dll.
 *
 * This is the file steam_api64.dll loads via HKCU\...\ActiveProcess\
 * SteamClientDll64 (#13). It presents Valve's flat C export set and MSVC-ABI
 * interface vtables to steam_api64.dll, and forwards every call across Wine's
 * __wine_unix_call seam into steamclient64.so, which drives the real
 * steamclient.dylib (shim_unix.cpp). It holds ZERO Steam logic — pure
 * marshalling — exactly like Proton's lsteamclient PE side (#3).
 *
 * The seam only bridges the CALLING CONVENTION: steam_api64.dll calls our
 * vtable thunks MS-x64 (this in RCX); each thunk packs a fixed-layout params
 * struct and crosses to the unix side, where the native call is made SysV. One
 * address space, one arch, so pointers and callback payloads pass verbatim.
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* The deploy contract (#32) — generated from src/layout/layout.json. */
#include "shim_paths.h"
#include "shim_policy.h"

#include "shim_abi.h"

/* i386 MSVC instance methods are __thiscall: `this` in ECX, remaining args on
 * the stack, and CALLEE-cleanup — the callee must pop exactly the bytes the
 * caller pushed. On x86_64 that same C signature already IS the convention
 * (this in RCX) and cleanup is the caller's, so THISCALL is empty there. Wine
 * draws the same line in include/wine/asm.h (DEFINE_THISCALL_WRAPPER is a no-op
 * off i386); gcc's thiscall attribute gives us the cleanup for free from the
 * declared signature, which is why every vtable thunk below must carry it. */
#if defined(__i386__)
# define THISCALL __attribute__((thiscall))
#else
# define THISCALL
#endif


typedef UINT64 unixlib_handle_t;
typedef LONG   NTSTATUS_T;
#define MemoryWineUnixFuncs 1000

typedef NTSTATUS_T (WINAPI *pNtQueryVirtualMemory)(HANDLE, PVOID, DWORD, PVOID, SIZE_T, SIZE_T *);
typedef NTSTATUS_T (WINAPI *pWineUnixCall)(unixlib_handle_t, unsigned int, void *);

static HMODULE          self_module;
static unixlib_handle_t g_handle;
static pWineUnixCall    g_unix_call;

/* OutputDebugStringA alone is not enough: CrossOver's launcher does not pass
 * WINEDEBUG through, so under the production launch path the PE side is silent
 * exactly when a title fails and you need to know which vtable slot it wanted.
 * Same lesson as #12's stderr-only launch log. Set SHIM_PE_LOG to a path.
 * The unix half already does this via SHIM_UNIX_LOG. */
static void dbg(const char *fmt, ...)
{
    static FILE *log;
    static int   tried;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    OutputDebugStringA(buf);
    if (!tried) {
        const char *p = getenv("SHIM_PE_LOG");
        tried = 1;
        if (p) log = fopen(p, "a");
    }
    if (log) { fprintf(log, "[%lu] %s\n", (unsigned long)GetCurrentProcessId(), buf); fflush(log); }
}

/* Wine reports a fault as a bare absolute address, and CrossOver strips the
 * trace channels that would name the module (+loaddll produces nothing here),
 * so the only way to attribute a crash is to have written the map down first.
 * Only runs when SHIM_PE_LOG is set — this is a diagnostic, not a hot path. */
static void dump_modules(void)
{
    HANDLE snap;
    MODULEENTRY32 me;
    if (!getenv("SHIM_PE_LOG")) return;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    me.dwSize = sizeof me;
    if (Module32First(snap, &me)) do {
        dbg("shim: module %p..%p  %s", me.modBaseAddr,
            (void *)(me.modBaseAddr + me.modBaseSize), me.szModule);
    } while (Module32Next(snap, &me));
    CloseHandle(snap);
}

static int ensure_seam(void)
{
    HMODULE ntdll;
    pNtQueryVirtualMemory query;
    SIZE_T ret_len = 0;
    if (g_unix_call && g_handle) return 0;
    ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return -1;
    query = (pNtQueryVirtualMemory)GetProcAddress(ntdll, "NtQueryVirtualMemory");
    g_unix_call = (pWineUnixCall)GetProcAddress(ntdll, "__wine_unix_call");
    if (!query || !g_unix_call) return -2;
    if (query(GetCurrentProcess(), (void *)self_module, MemoryWineUnixFuncs,
              &g_handle, sizeof(g_handle), &ret_len)) return -3;
    return 0;
}

static int seam(unsigned code, void *args)
{
    if (ensure_seam()) { dbg("shim: seam unavailable (not Wine / no .so)"); return -1; }
    return (int)g_unix_call(g_handle, code, args);
}

/* ---- per-thread scratch (#20) -------------------------------------------
 * gcc's __thread does NOT work here. This DLL is brought in by LoadLibrary at
 * runtime (steam_api resolves it from the SteamClientDll registry value), and a
 * thread that already existed when that happened has no slot for the module's
 * TLS block — reads come back as garbage. Explicit TlsAlloc/TlsGetValue has no
 * such rule: the slot is per-thread and allocated on first touch, whenever the
 * thread was born.
 *
 * NOTE: an earlier revision of this comment credited the switch with fixing the
 * Unity crash in GetCurrentGameLanguage. It did not, and the claim cost a lot of
 * time. That crash was in the dbg() call one line below the copy-down, which
 * formatted the RAW native pointer with %s instead of the copied-down string;
 * on i386 that truncates a >4 GB macOS heap address to 32 bits and the CRT's
 * strlen walks into it. The DLL that still crashed already had this TLS code in
 * it. Two independent i386 pointer-width bugs on the same line of source, and
 * fixing the harder one first hid the easier one — see FINDINGS-32BIT.md. */
#ifdef __i386__          /* only the i386 copy-down paths need per-thread scratch */
static DWORD g_tls = TLS_OUT_OF_INDEXES;

struct tls_scratch {
    char           str[4][1024];   /* rotating pool for const char* returns   */
    unsigned       turn;
    unsigned char *payload;        /* grown to fit the largest callback seen  */
    int            payload_cap;
};

static struct tls_scratch *tls_get(void)
{
    struct tls_scratch *t;
    if (g_tls == TLS_OUT_OF_INDEXES) return NULL;
    t = (struct tls_scratch *)TlsGetValue(g_tls);
    if (!t) {
        t = (struct tls_scratch *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *t);
        if (!t) return NULL;
        TlsSetValue(g_tls, t);
    }
    return t;
}

static void tls_free_current_thread(void)
{
    struct tls_scratch *t;
    if (g_tls == TLS_OUT_OF_INDEXES) return;
    t = (struct tls_scratch *)TlsGetValue(g_tls);
    if (!t) return;
    if (t->payload) HeapFree(GetProcessHeap(), 0, t->payload);
    HeapFree(GetProcessHeap(), 0, t);
    TlsSetValue(g_tls, NULL);
}
#else
static void tls_free_current_thread(void) { }
#endif

/* ---- native memory the game is handed BACK (#20) ------------------------
 * Direction matters. Anything the game passes IN is a PE address: it zero-
 * extends into the uint64 params-struct fields and the native side writes
 * through it, on both bitnesses, with no work. Anything the native side hands
 * BACK by pointer is different — it lives on the dylib's heap, which on macOS
 * sits well above 4 GB (0x7fd695b48ae0, from our own shim_unix.log). A 64-bit
 * PE holds that fine. A 32-bit PE cannot hold it at all, so the bytes must be
 * copied down across the seam before the game can look at them. */
#ifdef __i386__
/* Rotating pool: Valve's const char* returns are documented valid until the
 * next call, but games routinely hold two at once (language + country in one
 * log line). Four deep is cheap insurance; a single buffer is not. */
static const char *native_str(uint64_t native)
{
    struct tls_scratch *t = tls_get();
    struct sp_copystr p;
    char *buf;
    if (!native) return NULL;
    if (!t) { dbg("shim: native_str has no TLS scratch"); return NULL; }
    buf = t->str[t->turn++ & 3];
    p.src = native; p.dst = (uint64_t)(uintptr_t)buf; p.cap = (int32_t)sizeof t->str[0]; p.ret = 0;
    seam(C_CopyStr, &p);
    if (p.ret >= (int32_t)sizeof t->str[0])
        dbg("shim: native_str TRUNCATED %d -> %d bytes", p.ret, (int)sizeof t->str[0] - 1);
    return buf;
}
#else
static const char *native_str(uint64_t native) { return (const char *)(uintptr_t)native; }
#endif

/* ---- PE-side interface object: MSVC vtable ptr @0, opaque native handle @8 --
 * `desc` is the version descriptor this object's table belongs to, resolved once
 * at wrap() time. It is not part of the layout steam_api64.dll sees (nothing
 * outside this file reads past the vtable pointer); it exists because #78's
 * generated thunks all call native_slot() on every call, and without it that
 * would rescan all 212 descriptors to answer a question wrap() already knew. */
struct vt_desc;
struct w_iface { const void **vtable; uint64_t handle; const struct vt_desc *desc; };

/* The MSVC vtables the game sees: one table per interface VERSION, generated
 * slot-exact from Proton's lsteamclient (shim_vtables.h / gen_vtables.py, #20).
 *
 * #11 could serve every version of an interface from one hand-written table
 * whose unused slots were 0-arg stubs, because MS-x64 is caller-cleanup.
 * Neither shortcut survives i386:
 *   - thiscall is CALLEE-cleanup, so a stub must pop exactly the bytes its
 *     caller pushed. The generated stubs carry Proton's per-method arity.
 *   - layouts differ BY VERSION, not just by interface: SteamInput006 (Mars)
 *     inserts SetInputActionManifestFilePath at slot 2, shifting every later
 *     slot against SteamInput002 (Among Us). One table for both silently calls
 *     the wrong method (map trap 2).
 * So wiring below is resolved BY NAME against each version's own table, and a
 * name that is not in that version is reported rather than silently dropped. */
static unsigned int vt_unmapped(const char *version, int slot, const char *name);
#include "shim_vtables.h"

/* A title just called a slot nothing is wired into, and got 0 back.
 *
 * That 0 is the most expensive value in this file. It is not an error the caller
 * can see: FileExists answers "no such save", GetFileCount answers "no files",
 * BIsSubscribed answers "not owned" — each a plausible answer the title acts on.
 * Space Marine spent a whole session offering "New Campaign" over a complete
 * cloud save because of exactly this, and nothing anywhere said why (#43).
 *
 * dbg() alone was not enough: it writes to OutputDebugStringA, and to a file
 * only when SHIM_PE_LOG is set, so a normal run recorded nothing. So this also
 * crosses the seam into shim-unix.log, unconditionally — the whole point is to
 * be readable from a run nobody thought to instrument in advance.
 *
 * Once per (version, slot): a method called every frame would otherwise bury the
 * log it is supposed to be found in. Both strings come from the generated tables
 * and are static, so comparing pointers is the identity we want. Past the cap we
 * stop recording rather than stop logging — a truncated diagnosis is worse than
 * a repetitive one. */
static unsigned int vt_unmapped(const char *version, int slot, const char *name)
{
    static const char *seen_ver[256];
    static int seen_slot[256];
    static int nseen;
    struct sp_log p;
    char msg[320];
    int i;

    dbg("shim: %s slot %d %s (unmapped)", version, slot, name);

    for (i = 0; i < nseen; i++)
        if (seen_ver[i] == version && seen_slot[i] == slot) return 0;
    if (nseen < (int)(sizeof seen_ver / sizeof *seen_ver)) {
        seen_ver[nseen] = version; seen_slot[nseen] = slot; nseen++;
    }

    snprintf(msg, sizeof msg,
             "UNMAPPED %s::%s (slot %d) -> returned 0. The title called a method "
             "this shim has a correct table for but no thunk behind. The 0 is not "
             "an error it can see, so whatever it does next is built on a wrong "
             "answer. Wire it in shim_pe.c (#45).",
             version ? version : "(unknown)", name ? name : "(unknown)", slot);
    p.msg = (uint64_t)(uintptr_t)msg;
    seam(C_Log, &p);
    return 0;
}

static const struct vt_desc *vt_find(const char *ver)
{
    int i;
    if (!ver) return NULL;
    for (i = 0; g_vtdescs[i].version; i++)
        if (!strcmp(g_vtdescs[i].version, ver)) return &g_vtdescs[i];
    return NULL;
}

/* Wire one real thunk into a version's table by METHOD NAME. Returns 0 on
 * success. A miss is loud: on i386 a wrongly-placed thunk is a stack corruption,
 * not a wrong answer, so this must never fail quietly. */
/* Every ISteamClient typed getter does the same thing: hand back the interface
 * for a version string, which acquire() already resolves generically. Wiring
 * them one at a time means a title asking for one nobody wired gets NULL —
 * which is what left Among Us holding a NULL ISteamMatchmaking. It is a
 * multiplayer game, so it stops there, and reports it as a failed sign-in (#20).
 * Wire the whole family from the version's own table instead. Called BEFORE the
 * explicit wiring below, so a hand-written thunk still wins. */
static void *THISCALL ic_GetIface_UPV(struct w_iface *s, int32_t u, int32_t pipe, const char *ver);
static void *THISCALL ic_GetIface_PV(struct w_iface *s, int32_t pipe, const char *ver);

static void wire_getters(const char *ver)
{
    const struct vt_desc *d = vt_find(ver);
    int i;
    if (!d) { dbg("shim: wire_getters: no table for %s", ver); return; }
    for (i = 0; d->methods[i].name; i++) {
        const struct vt_method *m = &d->methods[i];
        const char *n = m->name;
        /* Valve retired some getters in place rather than removing the slot, so
         * the family is `GetISteam*` OR `DEPRECATED_GetISteam*`. Matching only
         * the first left DEPRECATED_GetISteamUnifiedMessages returning NULL from
         * a stub, which is indistinguishable from "no such interface" to a title
         * that still asks for it (ADR 0009). */
        if (!strncmp(n, "DEPRECATED_", 11)) n += 11;
        if (strncmp(n, "GetISteam", 9)) continue;
        if (m->bytes == 16)      d->vtable[m->slot] = (const void *)ic_GetIface_UPV;
        else if (m->bytes == 12) d->vtable[m->slot] = (const void *)ic_GetIface_PV;
        else dbg("shim: %s.%s unexpected shape (%u bytes) — left stubbed",
                 ver, m->name, (unsigned)m->bytes);
    }
}

static int wire(const char *ver, const char *method, const void *fn)
{
    const struct vt_desc *d = vt_find(ver);
    int i;
    if (!d) { dbg("shim: WIRE MISS %s (version not generated) .%s", ver, method); return -1; }
    for (i = 0; d->methods[i].name; i++)
        if (!strcmp(d->methods[i].name, method)) { d->vtable[d->methods[i].slot] = fn; return 0; }
    dbg("shim: WIRE MISS %s.%s (not in this version)", ver, method);
    return -1;
}

/* Look a method up in one version's table. */
static const struct vt_method *meth_find(const struct vt_desc *d, const char *method)
{
    int i;
    for (i = 0; d->methods[i].name; i++)
        if (!strcmp(d->methods[i].name, method)) return &d->methods[i];
    return NULL;
}

/* Which generated table does this wrapper carry? Every vt_desc owns exactly one
 * table, so the vtable pointer identifies the interface VERSION the title asked
 * for — and therefore the slot layout the native object on the other side of
 * the seam is using.
 *
 * This exists for the overlay activators (#23) and nothing else. Every other
 * forwarded method in this file happens to sit at the same slot in every
 * version it appears in, so the unix half can cast the native handle to one
 * fixed C++ class. ISteamFriends::ActivateGameOverlay does not: it lives at
 * slot 19, 20, 21, 22, 28 or 27 depending on the version. Wiring the PE side
 * by name is already correct — that is what wire_all does — but the NATIVE
 * call needs the same treatment, and only this side knows which version this
 * object is. So the slot travels with the call. */
static const struct vt_desc *vt_of(const void **vtable)
{
    int i;
    if (!vtable) return NULL;
    for (i = 0; g_vtdescs[i].version; i++)
        if (g_vtdescs[i].vtable == vtable) return &g_vtdescs[i];
    return NULL;
}

/* -1 means "could not name it", which the unix half treats as do-nothing-loudly
 * rather than dispatching through a guessed slot. There is no safe guess: the
 * wrong slot on ISteamFriends is a call to SetPlayedWith or GetClanTag.
 *
 * `native`, not `slot`. They are the same number for every method that is not
 * one of a same-name overload set, which is the overwhelming majority — but
 * MSVC lays an overload set out in REVERSE against the order the dylib was
 * compiled in, so for those the table this side calls through and the vtable
 * the other side indexes disagree. Sending `slot` for ISteamUserStats::GetStat
 * would call GetStat(const char*, float*) with an int32_t* — the right method
 * name, the wrong overload, and a write through a reinterpreted pointer.
 * gen_vtables.py resolves the correspondence; this just picks the right column
 * (#78). */
static int32_t native_slot(struct w_iface *s, const char *method)
{
    const struct vt_desc *d = s ? (s->desc ? s->desc : vt_of(s->vtable)) : NULL;
    const struct vt_method *m = d ? meth_find(d, method) : NULL;
    if (!m) {
        dbg("shim: native_slot(%s): unresolvable (version %s) — call dropped",
            method, d ? d->version : "unknown");
        return -1;
    }
    return (int32_t)m->native;
}

/* Wire a thunk into EVERY generated version of an interface that declares the
 * method with the SAME SHAPE as the version the thunk was written against.
 *
 * This is the difference between "the shim supports the titles we tested" and
 * "the shim supports the interface". Naming versions explicitly is why Space
 * Marine's SteamClient021 got a correct, slot-exact table in which every slot
 * was still a logging stub: the table existed, nothing was wired into it, and
 * the game died on CreateSteamPipe returning 0 (#29).
 *
 * `ref` is the version the thunk's C signature matches — the interface and the
 * expected shape both come from it, so a call site changes from wire(...) to
 * wire_all(...) and nothing else. Two safety properties, neither optional:
 *
 *   - Resolution is BY NAME against each version's own table, so a method that
 *     moved slots between versions still lands in the right slot.
 *   - A version whose `bytes` for that method differ from the reference is
 *     SKIPPED and logged, never wired. Same name does not mean same signature
 *     across versions, and on i386 (callee-cleanup) a wrongly-shaped thunk
 *     corrupts the caller's stack rather than returning a wrong answer. Those
 *     are the cases that still need an explicit per-version thunk, exactly as
 *     ISteamInput's Init/RunFrame already do.
 *
 * Returns how many versions were wired. */
static int wire_all(const char *ref, const char *method, const void *fn)
{
    const struct vt_desc *rd = vt_find(ref);
    const struct vt_method *rm;
    int n = 0, i;

    if (!rd) { dbg("shim: WIRE MISS %s (version not generated) .%s", ref, method); return -1; }
    rm = meth_find(rd, method);
    if (!rm) { dbg("shim: WIRE MISS %s.%s (not in the reference version)", ref, method); return -1; }

    for (i = 0; g_vtdescs[i].version; i++) {
        const struct vt_desc *d = &g_vtdescs[i];
        const struct vt_method *m;
        if (strcmp(d->iface, rd->iface)) continue;
        m = meth_find(d, method);
        if (!m) continue;                      /* absent in this version: fine */
        if (m->bytes != rm->bytes) {
            dbg("shim: %s.%s SHAPE MISMATCH (%u bytes vs %s's %u) — left stubbed",
                d->version, method, (unsigned)m->bytes, ref, (unsigned)rm->bytes);
            continue;
        }
        d->vtable[m->slot] = fn; n++;
    }
    return n;
}

/* Some methods changed SHAPE rather than slot between versions:
 * ISteamFriends::ActivateGameOverlayToWebPage gained a mode parameter at v017,
 * and ActivateGameOverlayToStore gained a flag at v013. Each shape needs its
 * own thunk — i386 thiscall is callee-cleanup, which is exactly the case
 * wire_all refuses to guess at — but wiring them with two wire_all calls makes
 * each pass report the OTHER pass's versions as "left stubbed". That turns the
 * one log line whose whole job is "here is a real gap" into noise, in a file
 * where the same line is currently telling the truth about ISteamUserStats.
 *
 * So wire both shapes in a single pass: a mismatch reported here is a version
 * that matches NEITHER, which is a genuine gap. Returns versions wired. */
static int wire_all_2(const char *method,
                      const char *refA, const void *fnA,
                      const char *refB, const void *fnB)
{
    const struct vt_desc *da = vt_find(refA), *db = vt_find(refB);
    const struct vt_method *ma = da ? meth_find(da, method) : NULL;
    const struct vt_method *mb = db ? meth_find(db, method) : NULL;
    int n = 0, i;

    if (!ma || !mb) {
        dbg("shim: WIRE MISS %s: reference version missing (%s=%s, %s=%s)", method,
            refA, ma ? "ok" : "no", refB, mb ? "ok" : "no");
        return -1;
    }
    if (ma->bytes == mb->bytes) {
        dbg("shim: %s: %s and %s are the SAME shape (%u bytes) — one wire_all "
            "would do; wiring %s only", method, refA, refB,
            (unsigned)ma->bytes, refA);
        return wire_all(refA, method, fnA);
    }
    for (i = 0; g_vtdescs[i].version; i++) {
        const struct vt_desc *d = &g_vtdescs[i];
        const struct vt_method *m;
        if (strcmp(d->iface, da->iface)) continue;
        m = meth_find(d, method);
        if (!m) continue;
        if      (m->bytes == ma->bytes) d->vtable[m->slot] = fnA;
        else if (m->bytes == mb->bytes) d->vtable[m->slot] = fnB;
        else {
            dbg("shim: %s.%s SHAPE MISMATCH (%u bytes, matches neither %s's %u "
                "nor %s's %u) — left stubbed", d->version, method,
                (unsigned)m->bytes, refA, (unsigned)ma->bytes, refB, (unsigned)mb->bytes);
            continue;
        }
        n++;
    }
    return n;
}

/* The GetISteam* family, for every version of an interface that has them. */
static void wire_getters_all(const char *iface)
{
    int i;
    for (i = 0; g_vtdescs[i].version; i++)
        if (!strcmp(g_vtdescs[i].iface, iface)) wire_getters(g_vtdescs[i].version);
}

/* small w_iface cache so repeated acquisition of the same native handle returns
 * a stable pointer (games and steam_api64.dll compare interface pointers). */
static struct { uint64_t handle; const void **vt; struct w_iface *w; } g_cache[32];
static int g_ncache;

static struct w_iface *wrap(uint64_t handle, const void **vt)
{
    int i;
    if (!handle || !vt) return NULL;   /* vt==NULL: no table for this version (i386) */
    for (i = 0; i < g_ncache; i++)
        if (g_cache[i].handle == handle && g_cache[i].vt == vt) return g_cache[i].w;
    struct w_iface *w = (struct w_iface *)HeapAlloc(GetProcessHeap(), 0, sizeof *w);
    w->vtable = vt; w->handle = handle; w->desc = vt_of(vt);
    if (g_ncache < 32) { g_cache[g_ncache].handle = handle; g_cache[g_ncache].vt = vt; g_cache[g_ncache].w = w; g_ncache++; }
    return w;
}

/* There used to be an x86_64 "generic stub" vtable here: 128 slots of a 0-arg
 * function returning 0, handed out for any version with no generated table. It
 * was safe in the narrow sense (MS-x64 is caller-cleanup, so the arity never
 * corrupted anything) and it is what #11 shipped.
 *
 * It is deleted because it answered the wrong question. A title that asks for
 * SteamClient021 and gets an object whose every method returns 0 does not stop
 * — it proceeds to build on the zeroes and null-dereferences somewhere deep in
 * its own init, with nothing anywhere naming the missing version. That is
 * exactly how Space Marine failed (#29), and the crash was indistinguishable
 * from the game not being launched with Steam at all, which cost several
 * control runs to tell apart.
 *
 * Both bitnesses now return NULL, which is Valve's documented "no such
 * interface" contract and the answer steam_api is written to handle. A missing
 * version is now one legible log line instead of a crash 20 frames away. */

/* Resolve a requested interface version to its slot-exact vtable. Matching is
 * on the FULL version string: "SteamInput002" and "SteamInput006" are different
 * layouts, and the old prefix match (any "SteamInput*" -> one table) is what
 * would have served Among Us the Mars layout (#20). */
static const void **vt_resolve(const char *ver)
{
    const struct vt_desc *d = vt_find(ver);
    if (d) return d->vtable;
    /* Loud, and named: this line is the whole diagnosis for a title that will
     * otherwise fail somewhere unrelated. */
    dbg("shim: *** NO VTABLE for interface version \"%s\" -> returning NULL. "
        "The title asked for a version this shim does not implement. Add it to "
        "interface-versions.txt and run ./build.sh --regen-vtables ***",
        ver ? ver : "(null)");
    return NULL;
}

/* ---- ISteamClient thunks ------------------------------------------------ */
static int32_t THISCALL ic_CreateSteamPipe(struct w_iface *s)
{ struct sp_client_noarg p; p.handle = s->handle; p.ret = 0; seam(C_Client_CreateSteamPipe, &p);
  dbg("shim: CreateSteamPipe -> %d", p.ret); return p.ret; }
static uint8_t THISCALL ic_BReleaseSteamPipe(struct w_iface *s, int32_t pipe)
{ struct sp_client_pipe p; p.handle = s->handle; p.pipe = pipe; p.ret = 0; seam(C_Client_BReleaseSteamPipe, &p); return (uint8_t)p.ret; }
static int32_t THISCALL ic_ConnectToGlobalUser(struct w_iface *s, int32_t pipe)
{ struct sp_client_pipe p; p.handle = s->handle; p.pipe = pipe; p.ret = 0; seam(C_Client_ConnectToGlobalUser, &p);
  dbg("shim: ConnectToGlobalUser(%d) -> %d", pipe, p.ret); return p.ret; }
static void THISCALL ic_ReleaseUser(struct w_iface *s, int32_t pipe, int32_t user)
{ struct sp_client_releaseu p; p.handle = s->handle; p.pipe = pipe; p.user = user; seam(C_Client_ReleaseUser, &p); }

/* Set_SteamAPI_CCheckCallbackRegisteredInProcess (#90).
 *
 * `func` is a PE address the native client would CALL, and the seam carries no
 * upcall — so it is dropped here deliberately rather than marshalled, and the
 * unix half registers its own always-1 stub in its place. Left unwired, this
 * slot answered with the refusal stub, and a client with no checker registered
 * has no reason to queue a callback for anyone.
 *
 * The parameter is still declared even though nothing reads it: on i386 this
 * vtable is callee-cleanup, so the thunk must pop exactly what the title pushed.
 * The native slot travels like the ISteamFriends overlay block's does — the
 * method moved through slots 30, 32, 33 and 34 across the versions that declare
 * it, and ISteamClient has no same-name overload, so the index the PE side
 * resolved is the index the dylib's own vtable uses. */
static void THISCALL ic_SetCheckCallbackRegistered(struct w_iface *s, void *func)
{ struct sp_client_checkcb p; p.handle = s->handle;
  p.slot = native_slot(s, "Set_SteamAPI_CCheckCallbackRegisteredInProcess");
  dbg("shim: Set_SteamAPI_CCheckCallbackRegisteredInProcess(%p) -> unix stub, slot %d",
      func, (int)p.slot);
  seam(C_Client_SetCheckCallbackRegistered, &p); }

static void *acquire(struct w_iface *s, int32_t user, int32_t pipe, const char *ver)
{
    struct sp_client_getgen p; p.handle = s->handle; p.ver = (uint64_t)(uintptr_t)ver;
    p.user = user; p.pipe = pipe; p.ret = 0;
    seam(C_Client_GetGeneric, &p);
    dbg("shim: acquire(\"%s\") native=0x%llx", ver ? ver : "(null)", (unsigned long long)p.ret);
    return wrap(p.ret, vt_resolve(ver));
}
/* The two shapes every ISteamClient typed getter comes in. Proton's byte count
 * tells them apart: 16 = (this, hUser, hPipe, version), 12 = (this, hPipe,
 * version) — the pipe-only form GetISteamUtils uses. */
static void * THISCALL ic_GetIface_UPV(struct w_iface *s, int32_t u, int32_t pipe, const char *ver) { return acquire(s, u, pipe, ver); }
static void * THISCALL ic_GetIface_PV(struct w_iface *s, int32_t pipe, const char *ver)             { return acquire(s, 0, pipe, ver); }

static void * THISCALL ic_GetISteamUser(struct w_iface *s, int32_t u, int32_t pipe, const char *ver)      { return acquire(s, u, pipe, ver); }
static void * THISCALL ic_GetISteamUtils(struct w_iface *s, int32_t pipe, const char *ver)                { return acquire(s, 0, pipe, ver); }
static void * THISCALL ic_GetISteamUserStats(struct w_iface *s, int32_t u, int32_t pipe, const char *ver) { return acquire(s, u, pipe, ver); }
static void * THISCALL ic_GetISteamApps(struct w_iface *s, int32_t u, int32_t pipe, const char *ver)      { return acquire(s, u, pipe, ver); }
static void * THISCALL ic_GetISteamFriends(struct w_iface *s, int32_t u, int32_t pipe, const char *ver)   { return acquire(s, u, pipe, ver); }
static void * THISCALL ic_GetISteamGenericInterface(struct w_iface *s, int32_t u, int32_t pipe, const char *ver) { return acquire(s, u, pipe, ver); }

/* ---- ISteamUser thunks -------------------------------------------------- */
static int32_t THISCALL iu_GetHSteamUser(struct w_iface *s)
{ struct sp_user_i32 p; p.handle = s->handle; p.ret = 0; seam(C_User_GetHSteamUser, &p); return p.ret; }
static uint8_t THISCALL iu_BLoggedOn(struct w_iface *s)
{ struct sp_user_i32 p; p.handle = s->handle; p.ret = 0; seam(C_User_BLoggedOn, &p); return (uint8_t)p.ret; }
/* GetSteamID returns CSteamID BY VALUE. MSVC uses the sret ABI for it: the caller
 * passes a hidden result-buffer pointer (rdx, after this=rcx) and expects the
 * callee to fill it and RETURN THAT POINTER in rax. The dylib (SysV) returns the
 * value in rax, so the seam value is correct; only this PE thunk must adapt.
 * Getting this wrong makes steam_api64 deref the SteamID as a pointer (page fault). */
static uint64_t * THISCALL iu_GetSteamID(struct w_iface *s, uint64_t *sret)
{ struct sp_user_u64 p; p.handle = s->handle; p.ret = 0; seam(C_User_GetSteamID, &p);
  *sret = p.ret; return sret; }

/* ---- ISteamUtils thunks ------------------------------------------------- */
static uint32_t THISCALL iut_GetAppID(struct w_iface *s)
{ struct sp_utils_u32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetAppID, &p);
  dbg("shim: GetAppID() -> %u", p.ret); return p.ret; }
static uint32_t THISCALL iut_GetSecondsSinceAppActive(struct w_iface *s)
{ struct sp_utils_u32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetSecondsSinceAppActive, &p); return p.ret; }
static uint32_t THISCALL iut_GetSecondsSinceComputerActive(struct w_iface *s)
{ struct sp_utils_u32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetSecondsSinceComputerActive, &p); return p.ret; }
static int32_t THISCALL iut_GetConnectedUniverse(struct w_iface *s)
{ struct sp_utils_i32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetConnectedUniverse, &p); return p.ret; }
static uint32_t THISCALL iut_GetServerRealTime(struct w_iface *s)
{ struct sp_utils_u32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetServerRealTime, &p); return p.ret; }
static const char * THISCALL iut_GetIPCountry(struct w_iface *s)
{ struct sp_utils_str p; const char *r; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetIPCountry, &p);
  r = native_str(p.ret);
  dbg("shim: GetIPCountry() -> %s", r ? r : "(null)");
  return r; }
static uint8_t THISCALL iut_GetCurrentBatteryPower(struct w_iface *s)
{ struct sp_utils_u32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetCurrentBatteryPower, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL iut_IsAPICallCompleted(struct w_iface *s, uint64_t call, void *failed)
{ struct sp_utils_call p; p.handle = s->handle; p.call = call; p.failed = (uint64_t)(uintptr_t)failed; p.ret = 0;
  seam(C_Utils_IsAPICallCompleted, &p); return (uint8_t)p.ret; }
static int32_t THISCALL iut_GetAPICallFailureReason(struct w_iface *s, uint64_t call)
{ struct sp_utils_callfail p; p.handle = s->handle; p.call = call; p.ret = 0;
  seam(C_Utils_GetAPICallFailureReason, &p); return p.ret; }
static uint8_t THISCALL iut_GetAPICallResult(struct w_iface *s, uint64_t call, void *buf, int32_t cub, int32_t expected, void *failed)
{ struct sp_utils_callres p; p.handle = s->handle; p.call = call; p.buf = (uint64_t)(uintptr_t)buf;
  p.cub = cub; p.expected = expected; p.failed = (uint64_t)(uintptr_t)failed; p.ret = 0;
  seam(C_Utils_GetAPICallResult, &p); return (uint8_t)p.ret; }
static void THISCALL iut_RunFrame(struct w_iface *s)
{ struct sp_utils_void p; p.handle = s->handle; seam(C_Utils_RunFrame, &p); }
static uint32_t THISCALL iut_GetIPCCallCount(struct w_iface *s)
{ struct sp_utils_u32 p; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetIPCCallCount, &p); return p.ret; }
static const char * THISCALL iut_GetSteamUILanguage(struct w_iface *s)
{ struct sp_utils_str p; const char *r; p.handle = s->handle; p.ret = 0; seam(C_Utils_GetSteamUILanguage, &p);
  r = native_str(p.ret);
  dbg("shim: GetSteamUILanguage() -> %s", r ? r : "(null)");
  return r; }

/* ---- ISteamUserStats (v012) thunks -------------------------------------- */
static uint8_t THISCALL is_RequestCurrentStats(struct w_iface *s)
{ struct sp_stats_noarg p; p.handle = s->handle; p.ret = 0; seam(C_Stats_RequestCurrentStats, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL is_GetAchievement(struct w_iface *s, const char *name, void *achieved)
{ struct sp_stats_getach p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.achieved = (uint64_t)(uintptr_t)achieved; p.ret = 0;
  seam(C_Stats_GetAchievement, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL is_SetAchievement(struct w_iface *s, const char *name)
{ struct sp_stats_name p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.ret = 0; seam(C_Stats_SetAchievement, &p);
  dbg("shim: SetAchievement(\"%s\") -> %d", name, p.ret); return (uint8_t)p.ret; }
static uint8_t THISCALL is_ClearAchievement(struct w_iface *s, const char *name)
{ struct sp_stats_name p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.ret = 0; seam(C_Stats_ClearAchievement, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL is_GetAchievementAndUnlockTime(struct w_iface *s, const char *name, void *achieved, void *unlock)
{ struct sp_stats_getachtime p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name;
  p.achieved = (uint64_t)(uintptr_t)achieved; p.unlock = (uint64_t)(uintptr_t)unlock; p.ret = 0;
  seam(C_Stats_GetAchievementAndUnlockTime, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL is_StoreStats(struct w_iface *s)
{ struct sp_stats_noarg p; p.handle = s->handle; p.ret = 0; seam(C_Stats_StoreStats, &p);
  dbg("shim: StoreStats() -> %d", p.ret); return (uint8_t)p.ret; }
static uint32_t THISCALL is_GetNumAchievements(struct w_iface *s)
{ struct sp_stats_u32ret p; p.handle = s->handle; p.ret = 0; seam(C_Stats_GetNumAchievements, &p); return p.ret; }
static const char * THISCALL is_GetAchievementName(struct w_iface *s, uint32_t idx)
{ struct sp_stats_nameidx p; p.handle = s->handle; p.idx = idx; p.ret = 0; seam(C_Stats_GetAchievementName, &p); return native_str(p.ret); }
static uint8_t THISCALL is_ResetAllStats(struct w_iface *s, uint8_t achievements_too)
{ struct sp_stats_reset p; p.handle = s->handle; p.achievements_too = achievements_too; p.ret = 0; seam(C_Stats_ResetAllStats, &p);
  dbg("shim: ResetAllStats(%d) -> %d", achievements_too, p.ret); return (uint8_t)p.ret; }
static const char * THISCALL is_GetAchievementDisplayAttribute(struct w_iface *s, const char *name, const char *key)
{ struct sp_stats_dispattr p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.key = (uint64_t)(uintptr_t)key; p.ret = 0;
  seam(C_Stats_GetAchievementDisplayAttribute, &p); return native_str(p.ret); }

/* ---- ISteamApps (VERSION008, slots 0-9) thunks -------------------------- */
static uint8_t THISCALL ia_BIsSubscribed(struct w_iface *s)
{ struct sp_apps_bool p; p.handle = s->handle; p.ret = 0; seam(C_Apps_BIsSubscribed, &p);
  dbg("shim: BIsSubscribed() -> %d", p.ret); return (uint8_t)p.ret; }
static uint8_t THISCALL ia_BIsLowViolence(struct w_iface *s)
{ struct sp_apps_bool p; p.handle = s->handle; p.ret = 0; seam(C_Apps_BIsLowViolence, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL ia_BIsCybercafe(struct w_iface *s)
{ struct sp_apps_bool p; p.handle = s->handle; p.ret = 0; seam(C_Apps_BIsCybercafe, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL ia_BIsVACBanned(struct w_iface *s)
{ struct sp_apps_bool p; p.handle = s->handle; p.ret = 0; seam(C_Apps_BIsVACBanned, &p); return (uint8_t)p.ret; }
static const char * THISCALL ia_GetCurrentGameLanguage(struct w_iface *s)
{ struct sp_apps_str p; const char *r; p.handle = s->handle; p.ret = 0; seam(C_Apps_GetCurrentGameLanguage, &p);
  r = native_str(p.ret);
  dbg("shim: GetCurrentGameLanguage() -> %s", r ? r : "(null)");
  return r; }
static const char * THISCALL ia_GetAvailableGameLanguages(struct w_iface *s)
{ struct sp_apps_str p; p.handle = s->handle; p.ret = 0; seam(C_Apps_GetAvailableGameLanguages, &p);
  return native_str(p.ret); }
static uint8_t THISCALL ia_BIsSubscribedApp(struct w_iface *s, uint32_t appid)
{ struct sp_apps_appid_bool p; p.handle = s->handle; p.appid = appid; p.ret = 0; seam(C_Apps_BIsSubscribedApp, &p);
  dbg("shim: BIsSubscribedApp(%u) -> %d", appid, p.ret); return (uint8_t)p.ret; }
static uint8_t THISCALL ia_BIsDlcInstalled(struct w_iface *s, uint32_t appid)
{ struct sp_apps_appid_bool p; p.handle = s->handle; p.appid = appid; p.ret = 0; seam(C_Apps_BIsDlcInstalled, &p); return (uint8_t)p.ret; }
static uint32_t THISCALL ia_GetEarliestPurchaseUnixTime(struct w_iface *s, uint32_t appid)
{ struct sp_apps_appid_u32 p; p.handle = s->handle; p.appid = appid; p.ret = 0; seam(C_Apps_GetEarliestPurchaseUnixTime, &p); return p.ret; }
static uint8_t THISCALL ia_BIsSubscribedFromFreeWeekend(struct w_iface *s)
{ struct sp_apps_bool p; p.handle = s->handle; p.ret = 0; seam(C_Apps_BIsSubscribedFromFreeWeekend, &p); return (uint8_t)p.ret; }
/* GetAppOwner returns CSteamID BY VALUE — same MSVC sret ABI as iu_GetSteamID
 * below: fill the caller's hidden buffer and return that pointer, not the value. */
static uint64_t * THISCALL ia_GetAppOwner(struct w_iface *s, uint64_t *sret)
{ struct sp_apps_u64 p; p.handle = s->handle; p.ret = 0; seam(C_Apps_GetAppOwner, &p);
  *sret = p.ret; return sret; }
static const char * THISCALL ia_GetLaunchQueryParam(struct w_iface *s, const char *key)
{ struct sp_apps_qparam p; p.handle = s->handle; p.key = (uint64_t)(uintptr_t)key; p.ret = 0;
  seam(C_Apps_GetLaunchQueryParam, &p); return native_str(p.ret); }

/* ---- ISteamUser (appended) ---------------------------------------------- */
/* EOS "Auth with Steam" (#20). Async: the SteamAPICall_t comes back here and the
 * ticket itself arrives later via EncryptedAppTicketResponse_t (1466) on the
 * existing pump, whereupon the game calls GetEncryptedAppTicket. Both buffers
 * belong to the game, so they are PE addresses that zero-extend across the
 * seam — no copy-down. Proton: slot 20 pops 12 bytes, slot 21 pops 16. */
static uint64_t THISCALL iu_RequestEncryptedAppTicket(struct w_iface *s, void *data, int32_t cb)
{ struct sp_user_reqticket p; p.handle = s->handle; p.data = (uint64_t)(uintptr_t)data;
  p.cb = cb; p.ret = 0; seam(C_User_RequestEncryptedAppTicket, &p);
  dbg("shim: RequestEncryptedAppTicket(cb=%d) -> call=%llu", cb, (unsigned long long)p.ret);
  return p.ret; }
static uint8_t THISCALL iu_GetEncryptedAppTicket(struct w_iface *s, void *ticket, int32_t max, uint32_t *cbticket)
{ struct sp_user_getticket p; p.handle = s->handle; p.ticket = (uint64_t)(uintptr_t)ticket;
  p.cbticket = (uint64_t)(uintptr_t)cbticket; p.max = max; p.ret = 0;
  seam(C_User_GetEncryptedAppTicket, &p);
  dbg("shim: GetEncryptedAppTicket(max=%d) -> %d (%u bytes)", max, p.ret, cbticket ? *cbticket : 0u);
  return (uint8_t)p.ret; }
static uint8_t THISCALL iu_GetUserDataFolder(struct w_iface *s, char *buf, int32_t len)
{ struct sp_user_datafolder p; p.handle = s->handle; p.buf = (uint64_t)(uintptr_t)buf; p.len = len; p.ret = 0;
  seam(C_User_GetUserDataFolder, &p); return (uint8_t)p.ret; }

/* ---- ISteamInput (VERSION006) thunks ------------------------------------ */
static uint8_t THISCALL iin_Init(struct w_iface *s, uint8_t explicit_runframe)
{ struct sp_input_init p; p.handle = s->handle; p.explicit_runframe = explicit_runframe; p.ret = 0;
  seam(C_Input_Init, &p); dbg("shim: SteamInput Init(%d) -> %d", explicit_runframe, p.ret);
  return (uint8_t)p.ret; }
static uint8_t THISCALL iin_Shutdown(struct w_iface *s)
{ struct sp_input_bool p; p.handle = s->handle; p.ret = 0; seam(C_Input_Shutdown, &p); return (uint8_t)p.ret; }
static void THISCALL iin_RunFrame(struct w_iface *s, uint8_t reserved)
{ struct sp_input_runframe p; p.handle = s->handle; p.reserved = reserved; seam(C_Input_RunFrame, &p); }
/* SteamInput002 declares Init()/RunFrame() with NO arguments; 006 added
 * bExplicitlyCallRunFrame / bReservedValue. Same method name, different
 * signature — so on i386 one thunk cannot serve both versions: it would pop 4
 * bytes the 002 caller never pushed. Slot order is not the only thing that
 * drifts between versions (#20). */
static uint8_t THISCALL iin_Init_002(struct w_iface *s)
{ struct sp_input_init p; p.handle = s->handle; p.explicit_runframe = 0; p.ret = 0;
  seam(C_Input_Init, &p); dbg("shim: SteamInput002 Init() -> %d", p.ret);
  return (uint8_t)p.ret; }
static void THISCALL iin_RunFrame_002(struct w_iface *s)
{ struct sp_input_runframe p; p.handle = s->handle; p.reserved = 0; seam(C_Input_RunFrame, &p); }

static uint8_t THISCALL iin_BNewDataAvailable(struct w_iface *s)
{ struct sp_input_bool p; p.handle = s->handle; p.ret = 0; seam(C_Input_BNewDataAvailable, &p); return (uint8_t)p.ret; }
static int32_t THISCALL iin_GetConnectedControllers(struct w_iface *s, uint64_t *out)
{ struct sp_input_handles p; p.handle = s->handle; p.out = (uint64_t)(uintptr_t)out; p.ret = 0;
  seam(C_Input_GetConnectedControllers, &p); return p.ret; }

/* ---- covering a title that relaunches itself (#27) -----------------------
 *
 * ADR 0003 covers children with DEBUG_PROCESS. That cannot work for the case
 * that actually occurs. Space Marine ships a 32-bit bootstrapper whose only job
 * is to start the 64-bit game, and a 32-bit debugger cannot debug a 64-bit
 * child — a hard rule on Windows, and Wine behaves the same. Measured: no
 * CREATE_PROCESS_DEBUG_EVENT arrives at all, the child gets the payload only
 * later through the ordinary steam_api route (by which time d3d12 and dxgi are
 * up), and the renderer logs zero Hooking lines.
 *
 * The parent's own CreateProcess is the one place that is guaranteed to run
 * before the child does anything, so hook it here. We are already inside the
 * parent as its first static import, so the hook is installed before the title
 * has executed a single instruction of its own.
 *
 * We do NOT patch the child ourselves: from a 32-bit parent the child's address
 * space is out of reach. We create it suspended and hand the pid to
 * overlayinject<child's bitness>.exe --attach, which does the patch in the
 * right bitness, then we resume. Only under SHIM_OVERLAY: with the overlay off
 * this must not alter how a title starts its own processes. */

typedef BOOL (WINAPI *pCreateProcessW_t)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
    LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
typedef BOOL (WINAPI *pCreateProcessA_t)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES,
    LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);

static pCreateProcessW_t g_real_cpW;
static pCreateProcessA_t g_real_cpA;

/* Read a PE's Machine field. Returns 1 for AMD64, 0 for anything else, and sets
 * *ok=0 when the file could not be read — in which case the caller assumes its
 * own bitness rather than guessing wrong. */
static int child_is_64bit_w(const wchar_t *exe, int *ok)
{
    HANDLE f;
    DWORD got = 0, pe = 0;
    WORD machine = 0;
    *ok = 0;
    if (!exe) return 0;
    f = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    SetFilePointer(f, 0x3c, NULL, FILE_BEGIN);
    if (ReadFile(f, &pe, sizeof pe, &got, NULL) && got == sizeof pe) {
        SetFilePointer(f, pe + 4, NULL, FILE_BEGIN);
        if (ReadFile(f, &machine, sizeof machine, &got, NULL) && got == sizeof machine) *ok = 1;
    }
    CloseHandle(f);
    return machine == 0x8664;
}

/* The child's image path: lpApplicationName when given, else the first token of
 * the command line, which may be quoted. */
static void child_exe_path(LPCWSTR app, LPCWSTR cmd, wchar_t *out, int cap)
{
    int i = 0;
    out[0] = 0;
    if (app && *app) { lstrcpynW(out, app, cap); return; }
    if (!cmd || !*cmd) return;
    if (*cmd == L'"') {
        cmd++;
        while (*cmd && *cmd != L'"' && i < cap - 1) out[i++] = *cmd++;
    } else {
        while (*cmd && *cmd != L' ' && i < cap - 1) out[i++] = *cmd++;
    }
    out[i] = 0;
}

/* Run overlayinject<bits>.exe --attach <pid> and wait for it. The helper lives
 * beside us — we are C:\shim\steamclient*.dll, it is C:\shim\overlayinject*.exe. */
static void attach_child(DWORD pid, int want64)
{
    wchar_t self[MAX_PATH], line[MAX_PATH + 64];
    wchar_t *slash;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    if (!GetModuleFileNameW(self_module, self, MAX_PATH)) return;
    slash = wcsrchr(self, L'\\');
    lstrcpyW(slash ? slash + 1 : self, want64 ? SHIM_PATH_INJECT64_W : SHIM_PATH_INJECT32_W);

    wsprintfW(line, L"\"%s\" --attach %lu", self, pid);
    dbg("shim: child pid=%lu is %d-bit -> %ls", pid, want64 ? 64 : 32, self);
    if (!CreateProcessW(self, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        dbg("shim: could not start the attach helper (%lu) — child has no overlay",
            GetLastError());
        return;
    }
    /* Wait: the child is suspended until the helper has patched it, and resuming
     * before that would be the race this whole mechanism exists to avoid. */
    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
}

static BOOL WINAPI hook_CreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa,
    LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env, LPCWSTR dir,
    LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
{
    int caller_wanted_suspended = (flags & CREATE_SUSPENDED) != 0;
    wchar_t exe[MAX_PATH];
    int ok = 0, is64;
    BOOL r = g_real_cpW(app, cmd, pa, ta, inherit, flags | CREATE_SUSPENDED, env, dir, si, pi);
    if (!r) return r;

    child_exe_path(app, cmd, exe, MAX_PATH);
    is64 = child_is_64bit_w(exe, &ok);
    if (!ok) is64 = (sizeof(void *) == 8);
    attach_child(pi->dwProcessId, is64);

    /* Restore the caller's semantics exactly: it only asked for a suspended
     * process if it said so. */
    if (!caller_wanted_suspended) ResumeThread(pi->hThread);
    return r;
}

static BOOL WINAPI hook_CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa,
    LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env, LPCSTR dir,
    LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi)
{
    int caller_wanted_suspended = (flags & CREATE_SUSPENDED) != 0;
    wchar_t exe[MAX_PATH];
    wchar_t wapp[MAX_PATH], wcmd[8192];
    int ok = 0, is64;
    BOOL r = g_real_cpA(app, cmd, pa, ta, inherit, flags | CREATE_SUSPENDED, env, dir, si, pi);
    if (!r) return r;

    wapp[0] = wcmd[0] = 0;
    if (app) MultiByteToWideChar(CP_ACP, 0, app, -1, wapp, MAX_PATH);
    if (cmd) MultiByteToWideChar(CP_ACP, 0, cmd, -1, wcmd, 8192);
    child_exe_path(app ? wapp : NULL, cmd ? wcmd : NULL, exe, MAX_PATH);
    is64 = child_is_64bit_w(exe, &ok);
    if (!ok) is64 = (sizeof(void *) == 8);
    attach_child(pi->dwProcessId, is64);

    if (!caller_wanted_suspended) ResumeThread(pi->hThread);
    return r;
}

/* Redirect one imported function in a module's IAT. Hooking the IAT rather than
 * the kernel32 export keeps the change local and reversible, and it is what the
 * loader has already resolved by the time the title calls anything. */
static int hook_iat_one(HMODULE mod, const char *want, const void *fn, void **orig)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)mod;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    DWORD rva;
    int hooked = 0;

    if (!mod || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS *)((BYTE *)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return 0;

    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)((BYTE *)mod + rva); imp->Name; imp++) {
        IMAGE_THUNK_DATA *othunk, *fthunk;
        if (!imp->OriginalFirstThunk) continue;
        othunk = (IMAGE_THUNK_DATA *)((BYTE *)mod + imp->OriginalFirstThunk);
        fthunk = (IMAGE_THUNK_DATA *)((BYTE *)mod + imp->FirstThunk);
        for (; othunk->u1.AddressOfData; othunk++, fthunk++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            DWORD old;
            if (othunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)((BYTE *)mod + othunk->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, want)) continue;
            if (!VirtualProtect(&fthunk->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) continue;
            if (orig && !*orig) *orig = (void *)(ULONG_PTR)fthunk->u1.Function;
            fthunk->u1.Function = (ULONG_PTR)fn;
            VirtualProtect(&fthunk->u1.Function, sizeof(void *), old, &old);
            hooked = 1;
        }
    }
    return hooked;
}

static void hook_child_creation(void)
{
    HMODULE exe = GetModuleHandleA(NULL);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    int w, a;

    /* Fall back to kernel32's own export if the exe imports CreateProcess
     * lazily or by ordinal, so the hook still has something real to call. */
    if (k32) {
        if (!g_real_cpW) g_real_cpW = (pCreateProcessW_t)GetProcAddress(k32, "CreateProcessW");
        if (!g_real_cpA) g_real_cpA = (pCreateProcessA_t)GetProcAddress(k32, "CreateProcessA");
    }
    if (!g_real_cpW && !g_real_cpA) { dbg("shim: no CreateProcess to hook"); return; }

    w = hook_iat_one(exe, "CreateProcessW", (const void *)hook_CreateProcessW, (void **)&g_real_cpW);
    a = hook_iat_one(exe, "CreateProcessA", (const void *)hook_CreateProcessA, (void **)&g_real_cpA);
    dbg("shim: child-creation hook installed (W=%d A=%d) — a title that relaunches "
        "itself will have its child injected too (#27)", w, a);
}

/* ---- ISteamFriends thunks (#29) ----------------------------------------- */
/* native_str is what makes this safe on i386: the dylib's heap sits above 4 GB,
 * so the returned pointer has to be copied down into PE memory before the game
 * can dereference it (the same rule as ia_GetCurrentGameLanguage). */
static const char * THISCALL ifr_GetPersonaName(struct w_iface *s)
{ struct sp_friends_str p; const char *r; p.handle = s->handle; p.ret = 0;
  seam(C_Friends_GetPersonaName, &p);
  r = native_str(p.ret);
  dbg("shim: GetPersonaName() -> %s", r ? r : "(null)");
  return r; }

/* ---- overlay activation (#23) -------------------------------------------
 *
 * These are the only methods in this file whose native slot is sent across the
 * seam (see native_slot above). Two of them come in two shapes across versions,
 * and each shape gets its own thunk for the same reason ISteamInput's Init
 * does: on i386 thiscall is callee-cleanup, so a thunk that pops the wrong
 * number of bytes corrupts the caller's stack rather than returning a wrong
 * answer. wire_all refuses to guess across a shape change, so we name both. */
static void THISCALL ifr_ActivateGameOverlay(struct w_iface *s, const char *dialog)
{ struct sp_fr_ov_str p; p.handle = s->handle; p.str = (uint64_t)(uintptr_t)dialog;
  p.slot = native_slot(s, "ActivateGameOverlay");
  dbg("shim: ActivateGameOverlay(\"%s\")", dialog ? dialog : "(null)");
  seam(C_Friends_ActivateOverlay, &p); }

static void THISCALL ifr_ActivateGameOverlayToUser(struct w_iface *s, const char *dialog, uint64_t steamid)
{ struct sp_fr_ov_user p; p.handle = s->handle; p.str = (uint64_t)(uintptr_t)dialog;
  p.steamid = steamid; p.slot = native_slot(s, "ActivateGameOverlayToUser");
  dbg("shim: ActivateGameOverlayToUser(\"%s\", %llu)", dialog ? dialog : "(null)",
      (unsigned long long)steamid);
  seam(C_Friends_ActivateOverlayToUser, &p); }

/* v017/v018: (pchURL, EActivateGameOverlayToWebPageMode). */
static void THISCALL ifr_ActivateGameOverlayToWebPage(struct w_iface *s, const char *url, int32_t mode)
{ struct sp_fr_ov_web p; p.handle = s->handle; p.url = (uint64_t)(uintptr_t)url;
  p.mode = mode; p.slot = native_slot(s, "ActivateGameOverlayToWebPage");
  dbg("shim: ActivateGameOverlayToWebPage(\"%s\", mode=%d)", url ? url : "(null)", mode);
  seam(C_Friends_ActivateOverlayToWebPage, &p); }

/* v005-v015: the URL alone, no mode. Mode 0 is k_EActivateGameOverlayToWebPage-
 * Mode_Default, and the native method of those versions has no second parameter
 * to be confused by it. */
static void THISCALL ifr_ActivateGameOverlayToWebPage_nomode(struct w_iface *s, const char *url)
{ struct sp_fr_ov_web p; p.handle = s->handle; p.url = (uint64_t)(uintptr_t)url;
  p.mode = 0; p.slot = native_slot(s, "ActivateGameOverlayToWebPage");
  dbg("shim: ActivateGameOverlayToWebPage(\"%s\") [no-mode form]", url ? url : "(null)");
  seam(C_Friends_ActivateOverlayToWebPage, &p); }

/* v013+: (nAppID, EOverlayToStoreFlag). */
static void THISCALL ifr_ActivateGameOverlayToStore(struct w_iface *s, uint32_t appid, int32_t flag)
{ struct sp_fr_ov_store p; p.handle = s->handle; p.appid = appid; p.flag = flag;
  p.slot = native_slot(s, "ActivateGameOverlayToStore");
  dbg("shim: ActivateGameOverlayToStore(%u, flag=%d)", appid, flag);
  seam(C_Friends_ActivateOverlayToStore, &p); }

/* v005-v012: the AppId alone. */
static void THISCALL ifr_ActivateGameOverlayToStore_noflag(struct w_iface *s, uint32_t appid)
{ struct sp_fr_ov_store p; p.handle = s->handle; p.appid = appid; p.flag = 0;
  p.slot = native_slot(s, "ActivateGameOverlayToStore");
  dbg("shim: ActivateGameOverlayToStore(%u) [no-flag form]", appid);
  seam(C_Friends_ActivateOverlayToStore, &p); }

static void THISCALL ifr_ActivateGameOverlayInviteDialog(struct w_iface *s, uint64_t lobby)
{ struct sp_fr_ov_id p; p.handle = s->handle; p.steamid = lobby;
  p.slot = native_slot(s, "ActivateGameOverlayInviteDialog");
  dbg("shim: ActivateGameOverlayInviteDialog(%llu)", (unsigned long long)lobby);
  seam(C_Friends_ActivateOverlayInviteDialog, &p); }

static void THISCALL ifr_ActivateGameOverlayRemotePlayTogetherInviteDialog(struct w_iface *s, uint64_t lobby)
{ struct sp_fr_ov_id p; p.handle = s->handle; p.steamid = lobby;
  p.slot = native_slot(s, "ActivateGameOverlayRemotePlayTogetherInviteDialog");
  dbg("shim: ActivateGameOverlayRemotePlayTogetherInviteDialog(%llu)", (unsigned long long)lobby);
  seam(C_Friends_ActivateOverlayRemotePlay, &p); }

static void THISCALL ifr_ActivateGameOverlayInviteDialogConnectString(struct w_iface *s, const char *connect)
{ struct sp_fr_ov_str p; p.handle = s->handle; p.str = (uint64_t)(uintptr_t)connect;
  p.slot = native_slot(s, "ActivateGameOverlayInviteDialogConnectString");
  dbg("shim: ActivateGameOverlayInviteDialogConnectString(\"%s\")", connect ? connect : "(null)");
  seam(C_Friends_ActivateOverlayConnectString, &p); }

static uint8_t THISCALL ifr_RegisterProtocolInOverlayBrowser(struct w_iface *s, const char *protocol)
{ struct sp_fr_ov_proto p; p.handle = s->handle; p.str = (uint64_t)(uintptr_t)protocol;
  p.slot = native_slot(s, "RegisterProtocolInOverlayBrowser"); p.ret = 0;
  seam(C_Friends_RegisterProtocolInOverlayBrowser, &p);
  dbg("shim: RegisterProtocolInOverlayBrowser(\"%s\") -> %d", protocol ? protocol : "(null)", p.ret);
  return (uint8_t)p.ret; }

/* ---- ISteamUtils overlay predicates (#23) --------------------------------
 *
 * No native handle crosses: the unix half answers these from Valve's renderer,
 * which is process state, not interface state. IsOverlayEnabled is the one the
 * whole ticket turns on — a title told `true` with nothing to draw pauses
 * forever, a title told `false` with an overlay running has dead buttons — so
 * it is deliberately NOT a constant and NOT tracked here. See the unix half. */
static uint8_t THISCALL iut_IsOverlayEnabled(struct w_iface *s)
{ struct sp_overlay_bool p; (void)s; p.ret = 0; seam(C_Overlay_IsEnabled, &p);
  dbg("shim: IsOverlayEnabled() -> %d", p.ret); return (uint8_t)p.ret; }
static uint8_t THISCALL iut_BOverlayNeedsPresent(struct w_iface *s)
{ struct sp_overlay_bool p; (void)s; p.ret = 0; seam(C_Overlay_BNeedsPresent, &p);
  return (uint8_t)p.ret; }
static void THISCALL iut_SetOverlayNotificationPosition(struct w_iface *s, int32_t pos)
{ struct sp_overlay_pos p; (void)s; p.pos = pos; seam(C_Overlay_SetNotificationPosition, &p); }
static void THISCALL iut_SetOverlayNotificationInset(struct w_iface *s, int32_t x, int32_t y)
{ struct sp_overlay_inset p; (void)s; p.x = x; p.y = y; seam(C_Overlay_SetNotificationInset, &p); }

/* ---- ISteamRemoteStorage thunks (slots 0-23) (#43) -----------------------
 *
 * The Steam Cloud file surface. A title with cloud saves does not read
 * userdata/<id>/<appid>/remote itself — it asks these, and a stubbed FileExists
 * makes a complete save invisible: Space Marine offers only "New Campaign" with
 * 22 KB of campaign progress sitting on disk, and cannot write a new save
 * either, because FileWrite is stubbed in the same breath.
 *
 * Buffers (`data`, `size_out`, the GetQuota out-params) are addresses the GAME
 * owns, so they zero-extend on i386 and the native side reads and writes through
 * them in place. GetFileNameAndSize is the one that cannot work that way: its
 * const char* RETURN points into the dylib's heap above 4 GB, so it goes through
 * native_str like GetIPCountry. */
static uint8_t THISCALL irs_FileWrite(struct w_iface *s, const char *name, const void *data, int32_t count)
{ struct sp_rs_filedata p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name;
  p.data = (uint64_t)(uintptr_t)data; p.count = count; p.ret = 0;
  seam(C_RS_FileWrite, &p); return (uint8_t)p.ret; }
static int32_t THISCALL irs_FileRead(struct w_iface *s, const char *name, void *data, int32_t count)
{ struct sp_rs_filedata p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name;
  p.data = (uint64_t)(uintptr_t)data; p.count = count; p.ret = 0;
  seam(C_RS_FileRead, &p); return p.ret; }
static uint64_t THISCALL irs_FileWriteAsync(struct w_iface *s, const char *name, const void *data, uint32_t count)
{ struct sp_rs_writeasync p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name;
  p.data = (uint64_t)(uintptr_t)data; p.count = (int32_t)count; p.ret = 0;
  seam(C_RS_FileWriteAsync, &p); return p.ret; }
static uint64_t THISCALL irs_FileReadAsync(struct w_iface *s, const char *name, uint32_t offset, uint32_t toread)
{ struct sp_rs_readasync p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name;
  p.offset = (int32_t)offset; p.toread = (int32_t)toread; p.ret = 0;
  seam(C_RS_FileReadAsync, &p); return p.ret; }
static uint8_t THISCALL irs_FileReadAsyncComplete(struct w_iface *s, uint64_t call, void *data, uint32_t toread)
{ struct sp_rs_readasyncdone p; p.handle = s->handle; p.call = call;
  p.data = (uint64_t)(uintptr_t)data; p.toread = (int32_t)toread; p.ret = 0;
  seam(C_RS_FileReadAsyncComplete, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL irs_FileForget(struct w_iface *s, const char *name)
{ struct sp_rs_name_i32 p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.ret = 0;
  seam(C_RS_FileForget, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL irs_FileDelete(struct w_iface *s, const char *name)
{ struct sp_rs_name_i32 p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.ret = 0;
  seam(C_RS_FileDelete, &p); return (uint8_t)p.ret; }
static uint64_t THISCALL irs_FileShare(struct w_iface *s, const char *name)
{ struct sp_rs_name_u64 p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.ret = 0;
  seam(C_RS_FileShare, &p); return p.ret; }
static uint8_t THISCALL irs_SetSyncPlatforms(struct w_iface *s, const char *name, int32_t platform)
{ struct sp_rs_syncplat p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name;
  p.platform = platform; p.ret = 0; seam(C_RS_SetSyncPlatforms, &p); return (uint8_t)p.ret; }
static uint64_t THISCALL irs_FileWriteStreamOpen(struct w_iface *s, const char *name)
{ struct sp_rs_name_u64 p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.ret = 0;
  seam(C_RS_FileWriteStreamOpen, &p); return p.ret; }
static uint8_t THISCALL irs_FileWriteStreamWriteChunk(struct w_iface *s, uint64_t stream, const void *data, int32_t count)
{ struct sp_rs_streamchunk p; p.handle = s->handle; p.stream = stream;
  p.data = (uint64_t)(uintptr_t)data; p.count = count; p.ret = 0;
  seam(C_RS_FileWriteStreamWriteChunk, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL irs_FileWriteStreamClose(struct w_iface *s, uint64_t stream)
{ struct sp_rs_stream p; p.handle = s->handle; p.stream = stream; p.ret = 0;
  seam(C_RS_FileWriteStreamClose, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL irs_FileWriteStreamCancel(struct w_iface *s, uint64_t stream)
{ struct sp_rs_stream p; p.handle = s->handle; p.stream = stream; p.ret = 0;
  seam(C_RS_FileWriteStreamCancel, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL irs_FileExists(struct w_iface *s, const char *name)
{ struct sp_rs_name_i32 p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.ret = 0;
  seam(C_RS_FileExists, &p);
  dbg("shim: FileExists(\"%s\") -> %d", name ? name : "(null)", p.ret);
  return (uint8_t)p.ret; }
static uint8_t THISCALL irs_FilePersisted(struct w_iface *s, const char *name)
{ struct sp_rs_name_i32 p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.ret = 0;
  seam(C_RS_FilePersisted, &p); return (uint8_t)p.ret; }
static int32_t THISCALL irs_GetFileSize(struct w_iface *s, const char *name)
{ struct sp_rs_name_i32 p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.ret = 0;
  seam(C_RS_GetFileSize, &p); return p.ret; }
static int64_t THISCALL irs_GetFileTimestamp(struct w_iface *s, const char *name)
{ struct sp_rs_name_i64 p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.ret = 0;
  seam(C_RS_GetFileTimestamp, &p); return p.ret; }
static int32_t THISCALL irs_GetSyncPlatforms(struct w_iface *s, const char *name)
{ struct sp_rs_name_i32 p; p.handle = s->handle; p.name = (uint64_t)(uintptr_t)name; p.ret = 0;
  seam(C_RS_GetSyncPlatforms, &p); return p.ret; }
static int32_t THISCALL irs_GetFileCount(struct w_iface *s)
{ struct sp_rs_noarg p; p.handle = s->handle; p.ret = 0; seam(C_RS_GetFileCount, &p);
  dbg("shim: GetFileCount() -> %d", p.ret); return p.ret; }
static const char * THISCALL irs_GetFileNameAndSize(struct w_iface *s, int32_t index, int32_t *size_out)
{ struct sp_rs_namesize p; const char *r; p.handle = s->handle;
  p.size_out = (uint64_t)(uintptr_t)size_out; p.index = index; p.ret = 0;
  seam(C_RS_GetFileNameAndSize, &p);
  r = native_str(p.ret);
  dbg("shim: GetFileNameAndSize(%d) -> %s", index, r ? r : "(null)");
  return r; }
static uint8_t THISCALL irs_GetQuota(struct w_iface *s, uint64_t *total, uint64_t *avail)
{ struct sp_rs_quota p; p.handle = s->handle; p.total = (uint64_t)(uintptr_t)total;
  p.avail = (uint64_t)(uintptr_t)avail; p.ret = 0; seam(C_RS_GetQuota, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL irs_IsCloudEnabledForAccount(struct w_iface *s)
{ struct sp_rs_noarg p; p.handle = s->handle; p.ret = 0;
  seam(C_RS_IsCloudEnabledForAccount, &p); return (uint8_t)p.ret; }
static uint8_t THISCALL irs_IsCloudEnabledForApp(struct w_iface *s)
{ struct sp_rs_noarg p; p.handle = s->handle; p.ret = 0;
  seam(C_RS_IsCloudEnabledForApp, &p);
  dbg("shim: IsCloudEnabledForApp() -> %d", p.ret); return (uint8_t)p.ret; }
static void THISCALL irs_SetCloudEnabledForApp(struct w_iface *s, int32_t enabled)
{ struct sp_rs_setcloud p; p.handle = s->handle; p.enabled = enabled;
  seam(C_RS_SetCloudEnabledForApp, &p); }

/* ---- generated thunks (#78) --------------------------------------------
 *
 * ~1,100 of them, one per (interface, method, SIGNATURE), emitted from the
 * typed wrapper bodies that sit one line below the arity in the same Proton
 * file the tables above come from. Everything they need is already defined:
 * seam(), native_str() for the const char* copy-down, native_slot() for the
 * index the unix half will use, and the vt_<version>[] tables themselves.
 *
 * They are deliberately NOT hand-editable. A method that needs different
 * behaviour goes in overrides.json with its reason, so the reason survives the
 * next regeneration; a patch to this header would not.
 *
 * What the generator declined to emit is in gen/REPORT.md, and every declined
 * slot still carries its logging stub — so the failure mode is a named line in
 * shim-unix.log (#45), never the silent 0 that cost #43 a whole session. */
#include "gen/shim_gen_pe.h"

static void build_vtables(void)
{
    vt_fill_stubs();

    /* First, so that anything hand-written below still wins the slot. Nothing
     * hand-written is generated (overrides.json, kept honest in both directions
     * by check_overrides.py), so in practice they do not overlap — this order
     * is what makes that a safety property rather than a coincidence. */
    dbg("shim: generated thunks wired into %d vtable slots", gen_wire_all());

    wire_getters_all("ISteamClient");

    /* Each wire_all names the version its thunk was written against; the
     * thunk then reaches every same-shaped version of that interface. */
    /* ISteamClient */
    wire_all("SteamClient017", "CreateSteamPipe", (const void *)ic_CreateSteamPipe);
    wire_all("SteamClient017", "BReleaseSteamPipe", (const void *)ic_BReleaseSteamPipe);
    wire_all("SteamClient017", "ConnectToGlobalUser", (const void *)ic_ConnectToGlobalUser);
    wire_all("SteamClient017", "ReleaseUser", (const void *)ic_ReleaseUser);
    wire_all("SteamClient017", "GetISteamUser", (const void *)ic_GetISteamUser);
    wire_all("SteamClient017", "GetISteamFriends", (const void *)ic_GetISteamFriends);
    wire_all("SteamClient017", "GetISteamUtils", (const void *)ic_GetISteamUtils);
    wire_all("SteamClient017", "GetISteamGenericInterface", (const void *)ic_GetISteamGenericInterface);
    wire_all("SteamClient017", "GetISteamUserStats", (const void *)ic_GetISteamUserStats);
    wire_all("SteamClient017", "GetISteamApps", (const void *)ic_GetISteamApps);
    wire_all("SteamClient017", "Set_SteamAPI_CCheckCallbackRegisteredInProcess",
             (const void *)ic_SetCheckCallbackRegistered);

    /* ISteamUser */
    wire_all("SteamUser021", "GetHSteamUser", (const void *)iu_GetHSteamUser);
    wire_all("SteamUser021", "BLoggedOn", (const void *)iu_BLoggedOn);
    wire_all("SteamUser021", "GetSteamID", (const void *)iu_GetSteamID);
    wire_all("SteamUser021", "GetUserDataFolder", (const void *)iu_GetUserDataFolder);
    wire_all("SteamUser021", "RequestEncryptedAppTicket", (const void *)iu_RequestEncryptedAppTicket);
    wire_all("SteamUser021", "GetEncryptedAppTicket", (const void *)iu_GetEncryptedAppTicket);

    /* ISteamUtils */
    wire_all("SteamUtils010", "GetSecondsSinceAppActive", (const void *)iut_GetSecondsSinceAppActive);
    wire_all("SteamUtils010", "GetSecondsSinceComputerActive", (const void *)iut_GetSecondsSinceComputerActive);
    wire_all("SteamUtils010", "GetConnectedUniverse", (const void *)iut_GetConnectedUniverse);
    wire_all("SteamUtils010", "GetServerRealTime", (const void *)iut_GetServerRealTime);
    wire_all("SteamUtils010", "GetIPCountry", (const void *)iut_GetIPCountry);
    wire_all("SteamUtils010", "GetCurrentBatteryPower", (const void *)iut_GetCurrentBatteryPower);
    wire_all("SteamUtils010", "GetAppID", (const void *)iut_GetAppID);
    wire_all("SteamUtils010", "IsAPICallCompleted", (const void *)iut_IsAPICallCompleted);
    wire_all("SteamUtils010", "GetAPICallFailureReason", (const void *)iut_GetAPICallFailureReason);
    wire_all("SteamUtils010", "GetAPICallResult", (const void *)iut_GetAPICallResult);
    wire_all("SteamUtils010", "RunFrame", (const void *)iut_RunFrame);
    wire_all("SteamUtils010", "GetIPCCallCount", (const void *)iut_GetIPCCallCount);
    wire_all("SteamUtils010", "GetSteamUILanguage", (const void *)iut_GetSteamUILanguage);
    /* The overlay predicates (#23), answered from the renderer rather than the
     * dylib — so they were the "our honest answer is the stub's false" case
     * until #25 made an overlay real, and are now the one place where a
     * hardcoded answer would be a hang. */
    wire_all("SteamUtils010", "IsOverlayEnabled", (const void *)iut_IsOverlayEnabled);
    wire_all("SteamUtils010", "BOverlayNeedsPresent", (const void *)iut_BOverlayNeedsPresent);
    wire_all("SteamUtils010", "SetOverlayNotificationPosition", (const void *)iut_SetOverlayNotificationPosition);
    wire_all("SteamUtils010", "SetOverlayNotificationInset", (const void *)iut_SetOverlayNotificationInset);

    /* ISteamUserStats */
    wire_all("STEAMUSERSTATS_INTERFACE_VERSION012", "RequestCurrentStats", (const void *)is_RequestCurrentStats);
    wire_all("STEAMUSERSTATS_INTERFACE_VERSION012", "GetAchievement", (const void *)is_GetAchievement);
    wire_all("STEAMUSERSTATS_INTERFACE_VERSION012", "SetAchievement", (const void *)is_SetAchievement);
    wire_all("STEAMUSERSTATS_INTERFACE_VERSION012", "ClearAchievement", (const void *)is_ClearAchievement);
    wire_all("STEAMUSERSTATS_INTERFACE_VERSION012", "GetAchievementAndUnlockTime", (const void *)is_GetAchievementAndUnlockTime);
    wire_all("STEAMUSERSTATS_INTERFACE_VERSION012", "StoreStats", (const void *)is_StoreStats);
    wire_all("STEAMUSERSTATS_INTERFACE_VERSION012", "GetAchievementDisplayAttribute", (const void *)is_GetAchievementDisplayAttribute);
    wire_all("STEAMUSERSTATS_INTERFACE_VERSION012", "GetNumAchievements", (const void *)is_GetNumAchievements);
    wire_all("STEAMUSERSTATS_INTERFACE_VERSION012", "GetAchievementName", (const void *)is_GetAchievementName);
    wire_all("STEAMUSERSTATS_INTERFACE_VERSION012", "ResetAllStats", (const void *)is_ResetAllStats);

    /* ISteamApps */
    wire_all("STEAMAPPS_INTERFACE_VERSION008", "BIsSubscribed", (const void *)ia_BIsSubscribed);
    wire_all("STEAMAPPS_INTERFACE_VERSION008", "BIsLowViolence", (const void *)ia_BIsLowViolence);
    wire_all("STEAMAPPS_INTERFACE_VERSION008", "BIsCybercafe", (const void *)ia_BIsCybercafe);
    wire_all("STEAMAPPS_INTERFACE_VERSION008", "BIsVACBanned", (const void *)ia_BIsVACBanned);
    wire_all("STEAMAPPS_INTERFACE_VERSION008", "GetCurrentGameLanguage", (const void *)ia_GetCurrentGameLanguage);
    wire_all("STEAMAPPS_INTERFACE_VERSION008", "GetAvailableGameLanguages", (const void *)ia_GetAvailableGameLanguages);
    wire_all("STEAMAPPS_INTERFACE_VERSION008", "BIsSubscribedApp", (const void *)ia_BIsSubscribedApp);
    wire_all("STEAMAPPS_INTERFACE_VERSION008", "BIsDlcInstalled", (const void *)ia_BIsDlcInstalled);
    wire_all("STEAMAPPS_INTERFACE_VERSION008", "GetEarliestPurchaseUnixTime", (const void *)ia_GetEarliestPurchaseUnixTime);
    wire_all("STEAMAPPS_INTERFACE_VERSION008", "BIsSubscribedFromFreeWeekend", (const void *)ia_BIsSubscribedFromFreeWeekend);
    wire_all("STEAMAPPS_INTERFACE_VERSION008", "GetAppOwner", (const void *)ia_GetAppOwner);
    wire_all("STEAMAPPS_INTERFACE_VERSION008", "GetLaunchQueryParam", (const void *)ia_GetLaunchQueryParam);

    /* ISteamFriends */
    wire_all("SteamFriends017", "GetPersonaName", (const void *)ifr_GetPersonaName);

    /* Overlay activation (#23). Two reference versions where the method changed
     * shape, so all fifteen versions that declare it are covered rather than
     * just the two a modern title asks for:
     *   ToWebPage — v017 added a mode parameter, v005-v015 have none
     *   ToStore   — v013 added a flag parameter, v005-v012 have none */
    wire_all("SteamFriends017", "ActivateGameOverlay", (const void *)ifr_ActivateGameOverlay);
    wire_all("SteamFriends017", "ActivateGameOverlayToUser", (const void *)ifr_ActivateGameOverlayToUser);
    wire_all_2("ActivateGameOverlayToWebPage",
               "SteamFriends017", (const void *)ifr_ActivateGameOverlayToWebPage,
               "SteamFriends015", (const void *)ifr_ActivateGameOverlayToWebPage_nomode);
    wire_all_2("ActivateGameOverlayToStore",
               "SteamFriends017", (const void *)ifr_ActivateGameOverlayToStore,
               "SteamFriends012", (const void *)ifr_ActivateGameOverlayToStore_noflag);
    wire_all("SteamFriends017", "ActivateGameOverlayInviteDialog", (const void *)ifr_ActivateGameOverlayInviteDialog);
    wire_all("SteamFriends017", "ActivateGameOverlayRemotePlayTogetherInviteDialog",
             (const void *)ifr_ActivateGameOverlayRemotePlayTogetherInviteDialog);
    wire_all("SteamFriends017", "ActivateGameOverlayInviteDialogConnectString",
             (const void *)ifr_ActivateGameOverlayInviteDialogConnectString);
    wire_all("SteamFriends017", "RegisterProtocolInOverlayBrowser",
             (const void *)ifr_RegisterProtocolInOverlayBrowser);

    /* ISteamInput stays PER-VERSION: 002 and 006 need genuinely different thunks
     * (different signatures), which is the case wire_all deliberately refuses to
     * guess at. */
    wire("SteamInput002", "Init", (const void *)iin_Init_002);
    wire("SteamInput006", "Init", (const void *)iin_Init);
    wire("SteamInput002", "Shutdown", (const void *)iin_Shutdown);
    wire("SteamInput006", "Shutdown", (const void *)iin_Shutdown);
    wire("SteamInput002", "RunFrame", (const void *)iin_RunFrame_002);
    wire("SteamInput006", "RunFrame", (const void *)iin_RunFrame);
    /* SteamInput002.BNewDataAvailable: not in this version */
    wire("SteamInput006", "BNewDataAvailable", (const void *)iin_BNewDataAvailable);
    wire("SteamInput002", "GetConnectedControllers", (const void *)iin_GetConnectedControllers);
    wire("SteamInput006", "GetConnectedControllers", (const void *)iin_GetConnectedControllers);

    /* ISteamRemoteStorage — the Cloud file surface, slots 0-23 (#43). One
     * wire_all per method reaches every generated version: slots 0-23 are
     * shape-identical from v001 to v016, so nothing here can land in the wrong
     * slot or pop the wrong bytes. Slots 24-58 (UGC/workshop/video) stay stubbed
     * on purpose — a different subsystem, and a stub there is a dead feature
     * rather than an invisible save. */
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "FileWrite", (const void *)irs_FileWrite);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "FileRead", (const void *)irs_FileRead);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "FileWriteAsync", (const void *)irs_FileWriteAsync);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "FileReadAsync", (const void *)irs_FileReadAsync);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "FileReadAsyncComplete", (const void *)irs_FileReadAsyncComplete);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "FileForget", (const void *)irs_FileForget);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "FileDelete", (const void *)irs_FileDelete);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "FileShare", (const void *)irs_FileShare);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "SetSyncPlatforms", (const void *)irs_SetSyncPlatforms);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "FileWriteStreamOpen", (const void *)irs_FileWriteStreamOpen);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "FileWriteStreamWriteChunk", (const void *)irs_FileWriteStreamWriteChunk);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "FileWriteStreamClose", (const void *)irs_FileWriteStreamClose);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "FileWriteStreamCancel", (const void *)irs_FileWriteStreamCancel);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "FileExists", (const void *)irs_FileExists);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "FilePersisted", (const void *)irs_FilePersisted);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "GetFileSize", (const void *)irs_GetFileSize);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "GetFileTimestamp", (const void *)irs_GetFileTimestamp);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "GetSyncPlatforms", (const void *)irs_GetSyncPlatforms);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "GetFileCount", (const void *)irs_GetFileCount);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "GetFileNameAndSize", (const void *)irs_GetFileNameAndSize);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "GetQuota", (const void *)irs_GetQuota);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "IsCloudEnabledForAccount", (const void *)irs_IsCloudEnabledForAccount);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "IsCloudEnabledForApp", (const void *)irs_IsCloudEnabledForApp);
    wire_all("STEAMREMOTESTORAGE_INTERFACE_VERSION016", "SetCloudEnabledForApp", (const void *)irs_SetCloudEnabledForApp);
}

/* ---- flat exports ------------------------------------------------------- */
__declspec(dllexport) void *CreateInterface(const char *name, int *returnCode)
{
    struct sp_create_interface p; p.name = (uint64_t)(uintptr_t)name; p.ret = 0;
    dbg("shim: CreateInterface(\"%s\")", name ? name : "(null)");
    seam(C_CreateInterface, &p);
    if (returnCode) *returnCode = p.ret ? 0 : 1;   /* 0 = k_EInterfaceOK */
    return wrap(p.ret, vt_resolve(name));
}

/* CallbackMsg_t is the one struct on this path that is NOT bitness-neutral,
 * because it carries a pointer:
 *
 *     win32 : { int user; int cb; uint8 *param; int cub; }   16 bytes, param @8
 *     x86_64: { int user; int cb; uint8 *param; int cub; }   24 bytes, param @8
 *
 * #11 could forward the game's own CallbackMsg_t straight through and let the
 * dylib fill it, because macOS x86_64 and Windows x64 agree byte for byte. A
 * 32-bit caller agrees on neither the size nor, more seriously, the payload
 * address: m_pubParam points into the dylib's heap above 4 GB. So on i386 we
 * hand the native side OUR 64-bit struct, then copy the payload down into PE
 * memory and hand the game a pointer it can actually dereference.
 *
 * The payload STRUCTS need no conversion: Valve picks callback packing by
 * PLATFORM, not bitness (PACK_SMALL on macOS, PACK_LARGE on Windows), so a
 * win32 game sees exactly what a win64 game sees — and 1101/1102/1103 are
 * byte-identical across all four ABIs regardless (#3 §6). */
#ifdef __i386__
struct cbmsg32 { int32_t user; int32_t cb; uint32_t param; int32_t cub; };
/* Trailing pad pins this at 24 whether the 32-bit compiler aligns uint64 to 4
 * or to 8; both put param @8 and cub @16, which is the x86_64 layout. */
struct cbmsg64 { int32_t user; int32_t cb; uint64_t param; int32_t cub; int32_t _pad; };
_Static_assert(sizeof(struct cbmsg64) == 24, "cbmsg64 must match the x86_64 CallbackMsg_t");
_Static_assert(sizeof(struct cbmsg32) == 16, "cbmsg32 must match the win32 CallbackMsg_t");

__declspec(dllexport) BOOL Steam_BGetCallback(int32_t pipe, void *msg)
{
    struct tls_scratch *t = tls_get();
    struct cbmsg64 m64;
    struct cbmsg32 *out = (struct cbmsg32 *)msg;
    struct sp_bgetcallback p;

    if (!out || !t) return FALSE;
    memset(&m64, 0, sizeof m64);
    p.pipe = pipe; p.msg = (uint64_t)(uintptr_t)&m64; p.ret = 0;
    seam(C_BGetCallback, &p);
    if (!p.ret) return FALSE;

    /* The payload stays alive until Steam_FreeLastCallback, so it is safe to
     * size the buffer from m_cubParam and copy in a second seam call rather
     * than guessing a capacity up front. */
    if (m64.param && m64.cub > 0) {
        if (m64.cub > t->payload_cap) {
            unsigned char *grown = (unsigned char *)(t->payload
                ? HeapReAlloc(GetProcessHeap(), 0, t->payload, m64.cub)
                : HeapAlloc(GetProcessHeap(), 0, m64.cub));
            if (!grown) { dbg("shim: callback payload alloc failed (%d bytes)", m64.cub); return FALSE; }
            t->payload = grown; t->payload_cap = m64.cub;
        }
        struct sp_copymem c;
        c.src = m64.param; c.dst = (uint64_t)(uintptr_t)t->payload; c.len = m64.cub;
        seam(C_CopyMem, &c);
        out->param = (uint32_t)(uintptr_t)t->payload;
    } else {
        out->param = 0;
    }
    out->user = m64.user; out->cb = m64.cb; out->cub = m64.cub;
    dbg("shim: callback %d (%d bytes) -> PE payload %p", m64.cb, m64.cub, (void *)(uintptr_t)out->param);
    return TRUE;
}
#else
__declspec(dllexport) BOOL Steam_BGetCallback(int32_t pipe, void *msg)
{
    struct sp_bgetcallback p; p.pipe = pipe; p.msg = (uint64_t)(uintptr_t)msg; p.ret = 0;
    seam(C_BGetCallback, &p);
    return p.ret ? TRUE : FALSE;
}
#endif
__declspec(dllexport) void Steam_FreeLastCallback(int32_t pipe)
{ struct sp_freelast p; p.pipe = pipe; seam(C_FreeLastCallback, &p); }
__declspec(dllexport) BOOL Steam_GetAPICallResult(int32_t pipe, uint64_t call, void *cb, int cub, int expected, void *failed)
{ struct sp_apicallresult p; p.pipe = pipe; p.call = call; p.cb = (uint64_t)(uintptr_t)cb; p.cub = cub;
  p.expected = expected; p.failed = (uint64_t)(uintptr_t)failed; p.ret = 0;
  seam(C_GetAPICallResult, &p); return p.ret ? TRUE : FALSE; }
__declspec(dllexport) void Steam_ReleaseThreadLocalMemory(int flag)
{ struct sp_release_tls p; p.flag = flag; seam(C_ReleaseThreadLocalMemory, &p); }

/* steam_api64.dll probes these; benign answers keep it from bailing. */
__declspec(dllexport) BOOL Steam_IsKnownInterface(const char *ver) { (void)ver; return TRUE; }
__declspec(dllexport) void Steam_NotifyMissingInterface(int32_t pipe, const char *ver) { (void)pipe; dbg("shim: NotifyMissingInterface(\"%s\")", ver ? ver : "?"); }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    (void)reserved;
    /* Thread notifications are deliberately NOT disabled any more: the per-thread
     * scratch (tls_get) is freed on DLL_THREAD_DETACH. DisableThreadLibraryCalls
     * would save a few notifications and leak a block per game thread instead. */
    if (reason == DLL_THREAD_DETACH) { tls_free_current_thread(); return TRUE; }
    if (reason == DLL_PROCESS_DETACH) {
        tls_free_current_thread();
#ifdef __i386__
        if (g_tls != TLS_OUT_OF_INDEXES) { TlsFree(g_tls); g_tls = TLS_OUT_OF_INDEXES; }
#endif
        return TRUE;
    }
    if (reason == DLL_PROCESS_ATTACH) {
        self_module = (HMODULE)inst;
#ifdef __i386__
        g_tls = TlsAlloc();
        if (g_tls == TLS_OUT_OF_INDEXES) dbg("shim: TlsAlloc failed");
#endif
        build_vtables();
        /* Overlay (#25 / ADR 0003). When the injector puts us into a process at
         * creation, merely being loaded must be enough — nothing here will ever
         * call a seam entry point, because the title may not use Steam at all.
         * Binding the seam is what makes ntdll dlopen our unixlib, and the
         * unixlib's constructor is what dlopens Valve's renderer, so this one
         * call is the whole payload.
         *
         * It has to happen in DllMain, under loader lock, because the deadline
         * is the process's first USER call (winemac.so is demand-loaded — see
         * the study's Addendum 2 B7). ensure_seam only does GetModuleHandle,
         * GetProcAddress and NtQueryVirtualMemory: no LoadLibrary, so it is not
         * the loader-lock hazard that a LoadLibrary here would be. */
        /* The predicate comes from the manifest (#33). What stood here was
         * GetEnvironmentVariableA(..., NULL, 0) > 0, which asks whether the
         * variable EXISTS — and it returns 2 for the string "0". The launch
         * script's no-injector branch exports exactly that, and the installer
         * bakes a literal 0 or 1 into the launcher, so the one configuration
         * the comment above insists must not run this ran it. */
        if (shim_overlay_enabled()) {
            int rc = ensure_seam();
            /* Cover a title that starts the real game in another process (#27).
             * Installed here, under loader lock, because that is still before
             * the title's own entry point — and it only touches our own IAT
             * page, with no LoadLibrary. */
            hook_child_creation();
            /* Diagnostic for #25: whether the title's own startup has already
             * run by the time we initialise. user32 loaded here means the PE
             * loader has already processed the title's static imports — and if
             * any of those touched USER, winemac.so (and NSApp) beat us. */
            dbg("shim: overlay pre-bind, ensure_seam=%d user32=%s d3d12=%s dxgi=%s", rc,
                GetModuleHandleA("user32.dll") ? "LOADED" : "no",
                GetModuleHandleA("d3d12.dll") ? "LOADED" : "no",
                GetModuleHandleA("dxgi.dll") ? "LOADED" : "no");
            {
                FILE *f = fopen("C:\\overlayinject.log", "a");
                if (f) {
                    fprintf(f, "  payload DllMain: ensure_seam=%d user32=%s d3d12=%s dxgi=%s\n", rc,
                            GetModuleHandleA("user32.dll") ? "LOADED" : "no",
                            GetModuleHandleA("d3d12.dll") ? "LOADED" : "no",
                            GetModuleHandleA("dxgi.dll") ? "LOADED" : "no");
                    fclose(f);
                }
            }
        }
        /* Base address is load-bearing diagnostics: a wine fault is reported as
         * an absolute address, and the only way to tell whether it is ours is to
         * subtract our base and disassemble the offset (#11's method). */
        dump_modules();
        dbg("shim: %s attached, base=%p",
            sizeof(void *) == 8 ? SHIM_PATH_PE64 : SHIM_PATH_PE32, (void *)inst);
    }
    return TRUE;
}
