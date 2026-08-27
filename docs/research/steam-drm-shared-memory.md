---
status: current
re-verify-on: a Steam client update (the shared-memory object names are the Windows client's, and Valve can rename them), or a Space Marine II update (the DRM wrapper is re-applied per build) — everything below is pinned to appid 2183900 buildid 24668625, CrossOver 26.3.0, macOS 26.5.2, measured 2026-08-27
---

# Steam DRM requires a Valve-signed `steamclient64.dll`

> **Resolved and shipped.** The route is `src/drm/` and [ADR 0014](../adr/0014-drm-titles-run-through-valves-own-signed-client-dll.md);
> this document is the measurement behind it, including four candidates that were built and
> eliminated first.

**A DRM-wrapped title fails before it calls a single Steamworks method, and the failure looks
exactly like a method-coverage gap without being one.** This document is the measured chain,
from the wrapper's first call into our shim to the error dialog 3 ms later.

The title is Warhammer 40,000: Space Marine II (2183900). It is the first DRM-wrapped title
this project has exercised, and nothing in `#45`'s residue is involved.

Claims are marked:

- **[MEASURED]** — produced by this machine, with the command that produced it.
- **[OBSERVED]** — reproducible behaviour, not quantified.
- **[SPECULATIVE]** — inference. Not measured.

---

## Summary

**[MEASURED]** The wrapper uses `CreateInterface` only to *locate the file that provides it*.
It then reads that file, discards it, and asks for two named kernel objects that a running
**Windows** Steam client would own. Both are absent. It shows the dialog and stops.

It never calls a method on either interface it was handed.

That is the whole failure, and it is upstream of everything `#45` tracks. **No amount of method
coverage can move it.**

**[MEASURED]** RESOLVED. The `SteamStart_*` probe is the wrapper's **error-reporting channel**, not
its check: answering it removes the dialog and changes nothing else. The actual gate is a
**Valve RSA-1024 code signature on `steamclient64.dll`**, verified against keys hard-coded in the
title. Proton passes it by loading Valve's *real* signed DLL and trampolining its exports into
`lsteamclient` — a route that forges nothing. **Reproduced here: Space Marine II launches.** See *Resolved: the wrapper requires a Valve RSA signature* below — that section supersedes
the reasoning in the two that precede it, which are kept because the eliminations are still sound.

---

## The chain

**[MEASURED]** `WINEDEBUG=+relay`, one thread, `402013.971` → `402013.974`:

```
GetProcAddress(steamclient64, "CreateInterface")
GetModuleHandleExA(FROM_ADDRESS|UNCHANGED_REFCOUNT, <that pointer>)
RtlPcToFileHeader()                    -> 0x6ffffb1f0000
GetModuleFileNameW()                   -> "C:\shim\steamclient64.dll"
CreateFileW("C:\shim\steamclient64.dll", GENERIC_READ)
GetFileSize()                          -> 0x19614D            (1,663,309 bytes)
RtlAllocateHeap(0x19614D)
ReadFile()                             -> reads the whole DLL
HeapFree()                             -> and discards it

OpenEventA("Local\SteamStart_SharedMemLock")        -> c0000034  OBJECT_NAME_NOT_FOUND
OpenFileMappingA("Local\SteamStart_SharedMemFile")  -> c0000034  OBJECT_NAME_NOT_FOUND

GetProcAddress(user32, "MessageBoxA")
MessageBoxA(0, "Application load error 3:0000065432", "Steam Error", 0x10)
```

`0x19614D` is the exact byte size of the shim built that day, which is what identifies the file
being read as ours rather than any other module.

### Where the verdict is actually formed

**[MEASURED]** The file read is **not** the fatal step. The wrapper reads the DLL, frees the
buffer, and proceeds. The two `c0000034` results are what it branches on: twelve calls separate
the failed `OpenFileMappingA` from `MessageBoxA`, with nothing in between but the `user32` load
and the `GetProcAddress` for the box itself.

**[SPECULATIVE]** What the read is *for* is unknown. It is consistent with hashing, with reading
the PE headers, or with nothing at all. It does not change the outcome here, so it was not chased.

---

## Why it looks like a coverage gap and is not

**[MEASURED]** Across every run, in both `shim-unix.log` and the `SHIM_PE_LOG` PE-side log:

| checked | result |
|---|---|
| `UNMAPPED` lines from this title | **none** — the only ones in the log are `SetWarningMessageHook`, from other titles |
| `*** NO VTABLE for interface version ***` | **none** — both requested versions resolved |
| vtable methods dispatched | **none** — not one, on either interface |
| `CreateInterface` return values | **valid** — `-> 0x2D5BF0 (handle=223447de8)` and `-> 0x2D5C20 (handle=223447dd0)` |
| flat exports called | **none** — 18 of the 34 missing ones were added as logging stubs; nothing called them |
| signature / crypto APIs called | **none** — `WINEDEBUG=+wintrust,+chain,+crypt` produced 2 lines and no verification call |

The title asks for `SteamClient017`, then `SteamClient014`, from the same routine 41 bytes apart.
Both callers (`0x01E36694`, `0x01E366BD`) are **below the lowest loaded module base**
(`0x140000000`, the game's own EXE) and inside none of the 60 mapped modules: the wrapper unpacks
itself into allocated memory and runs from there, before the game's code.

The `SteamClient014` request is the tell. **[SPECULATIVE]** Nothing calls that interface, so the
version asked for is probably a fingerprint of the wrapper's vintage rather than a statement of
what it intends to use.

---

## What this collides with

`CONTEXT.md`: *"the bridge connects to it over Valve's own IPC; **no Windows Steam runs**. This
'no Windows Steam' property is the whole point of the effort."*

`Local\SteamStart_SharedMemFile` and `Local\SteamStart_SharedMemLock` are created by the Windows
Steam client. The DRM's handshake is out-of-band — not through `steamclient64.dll` at all — and
it is looking for precisely the process this project exists to not run.

**[SPECULATIVE]** Whether that is satisfiable is an open question, and the deciding fact is what
the mapping *contains*. A presence advertisement that the real, authenticated macOS client could
genuinely back is the same interoperability the shim already performs on a different channel. An
ownership or decryption ticket that only the Windows client can mint is not — producing one would
be forging the answer rather than forwarding it. **The protocol has not been read, and until it
has, neither claim should be made.** See #97.

---

## It asks for the active client's pid first, and we do not write one

**[MEASURED]** Before it ever loads our DLL, the wrapper reads the *same registry key our launch
script writes to*, three times in 1 ms:

```
RegOpenKeyExW(HKCU, L"Software\\Valve\\Steam\\ActiveProcess")
RegQueryValueExW(L"pid")               -> 2   ERROR_FILE_NOT_FOUND
RegQueryValueExW(L"SteamClientDll64")  -> 0   OK   (the value we plant)
RegQueryValueExW(L"pid")               -> 2   ERROR_FILE_NOT_FOUND
```

`SteamClientDll64` is ours and answers. `pid` is **not written by anything in this project** and
does not exist. The wrapper asks for it twice, on either side of the value that leads it to our DLL.

**[MEASURED]** Proton writes exactly this value: `steam_helper/steam.c` calls
`RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"pid", ...)`
with its own process id.

**[SPECULATIVE]** Whether supplying it changes the outcome is untested, and it cannot be tested
honestly by writing an arbitrary number: the value names a process the wrapper can presumably open.
Proton's answer points at a real in-prefix process that represents the real client. That is a
feature, not a registry write — see the scope question below and #97.

---

## What Valve themselves do, and the part that does not add up

**[MEASURED]** Proton solves this problem in the open. `steam_helper/steam.c` (`proton_11.0`) is a
Windows `steam.exe` that runs *inside* the prefix and represents the real, host-side Steam client:

| surface | what it creates |
|---|---|
| registry | `HKCU\Software\Valve\Steam\ActiveProcess\pid`, `HKLM\SOFTWARE\Valve\Steam\Apps` (32-bit view) |
| windows | a registered class with a window titled `"Steam"`, plus a `"SteamVR Status"` static |
| events | `Steam3Master_SharedMemLock`, `Global\Valve_SteamIPC_Class` |
| semaphores | `STEAM_DIPC_CONSUME`, `SREAM_DIPC_PRODUCE` (Valve's typo, preserved) |
| handshake | `steam_drm_thread` waits on consume, finds `STEAM_START_ACK_EVENT*` by enumerating `\BaseNamedObjects\Session\1`, signals it, releases produce |
| env | `SteamPath`, `ValvePlatformMutex` |

That is the shape of the answer, if there is one: **not a forged client, but an in-prefix proxy for
a real, running, authenticated one.** Structurally it is what this project already does for the
Steamworks API, on a channel we do not currently serve.

### The part that does not add up

**[MEASURED]** Proton creates **none** of the two objects our title asks for. There is no
`SteamStart_SharedMemFile` and no `SteamStart_SharedMemLock` anywhere in `steam.c`.

**[OBSERVED]** And yet Space Marine II is widely reported working under Proton with no tinkering.

So one of these is true, and **this document does not establish which**:

1. Under Proton the wrapper never reaches this check, because something earlier satisfies it.
2. The `SteamStart_*` pair is serviced by something other than `steam_helper`.
3. The wrapper takes a different branch when its parent process is Steam's own helper.

### Resolved: 1

**[MEASURED]** `SteamStart` appears in `steam_helper/steam.c` on **none** of `proton_9.0`,
`proton_10.0`, `proton_11.0`, `experimental_11.0` or `bleeding-edge`, and a code search across the
whole `ValveSoftware` org returns nothing. Proton does not create these objects at any version.

**[OBSERVED]** Proton's own tracking issue for this title, ValveSoftware/Proton#8072, is entirely
EAC runtime, EOS, crossplay and Steam-overlay crashes. Nobody reports `Application load error`.

**Therefore the wrapper does not reach this probe under Proton.** Something earlier answers.

**[MEASURED]** The only earlier Steam-presence probe in our trace is the `pid` read above, and it
fails here (`ERROR_FILE_NOT_FOUND`, twice) while Proton sets that value. That is the one measured
difference between the two environments on the path to this check.

**[SPECULATIVE]** — the hypothesis this supported was that the `pid` read is the gate and
`SteamStart_*` is the fallback taken when no live client is found. **Since refuted by measurement**
— see *Proton's full presence set is not enough either* below. The probe is unconditional. What
survives is the narrower claim above: `pid` was the one measured difference on the path to it, and
supplying it does not change the path.

### Corrections

Two, in opposite directions, which is why both are kept.

**1.** An earlier revision asserted that `SteamStart_*` was "the *primary* check, not a fallback",
because no earlier probe had failed. That was wrong at the time: the claim rested on a grep for
**named kernel objects only**, which cannot see a registry read. `ActiveProcess\pid` is an earlier
Steam-presence probe, it did fail, and it was sitting in the trace the whole time.

**2.** The correction to that — "`pid` is the gate, `SteamStart_*` is its fallback" — was also
wrong, and this time the measurement exists to say so. With `pid` published, opened and
liveness-checked, the `SteamStart_*` probe is issued from the same instruction as when `pid` is
absent entirely. The original "primary check, not a fallback" reading was right about the
conclusion and wrong about the evidence; it is right now for a reason that was not available then.

---

## Proton's full presence set is not enough either

**[MEASURED]** 2026-08-27, second session. A throwaway stub published *everything* Proton's
`steam_helper/steam.c` publishes — the whole table above — inside the `steam-shim` bottle, and
stayed alive:

| surface | published |
|---|---|
| registry | `ActiveProcess\pid` = the stub's own pid |
| windows | class `vguiPopupWindow`, window titled `"Steam"`; static `"SteamVR Status"` |
| events | `Steam3Master_SharedMemLock`, `Global\Valve_SteamIPC_Class` |
| semaphores | `STEAM_DIPC_CONSUME` (0/512), `SREAM_DIPC_PRODUCE` (1/512) |
| thread | `steam_drm_thread` — waits on consume, would find and signal `STEAM_START_ACK_EVENT*` |
| env | `SteamPath`, `ValvePlatformMutex` |

Run twice: once with the title launched separately, once with the stub as the title's **parent**,
which is Proton's topology. **Both fail identically.** The PE-side log stops at the same 82 lines
and the same two `CreateInterface` calls, with no method dispatched.

### The presence check now passes, and it changes nothing

**[MEASURED]** The wrapper engages with the presence set completely, then ignores the result:

```
RegQueryValueExW(L"pid")              -> 0          (was 2 — ERROR_FILE_NOT_FOUND)
OpenProcess(0x400, pid=0xec)          -> 0xb4
GetExitCodeProcess(0xb4)              -> 1          STILL_ACTIVE
RegQueryValueExW(L"SteamClientDll")   -> 0
RegQueryValueExW(L"SteamClientDll64") -> 0
RegQueryValueExW(L"pid")              -> 0          (asked a second time)
OpenProcess(0x400, pid=0xec)          -> 0xb4       (opened a second time)
GetExitCodeProcess(0xb4)              -> 1          (liveness confirmed a second time)
...
CreateFileW(L"C:\shim\steamclient64.dll")           read and discarded, as before
OpenEventA("Local\SteamStart_SharedMemLock")        -> c0000034  OBJECT_NAME_NOT_FOUND
OpenFileMappingA("Local\SteamStart_SharedMemFile")  -> c0000034  OBJECT_NAME_NOT_FOUND
MessageBoxA(0, "Application load error 3:0000065432", "Steam Error", 0x10)
```

**[MEASURED]** The `SteamStart_*` probe is issued from `0x14551643d` — **byte-identical across all
three runs**: no `pid` at all, `pid` only, and the full presence set with a parent stub. The call
site does not move, and nothing between the satisfied liveness check and the probe touches any
other Steam surface (22 `OutputDebugStringA`, 3 `CreateFileW`, one `LoadLibraryExW` of our own
shim, and our shim's own export-name `strncmp` sweep — nothing else).

**The `SteamStart_*` probe is unconditional.** It is not a fallback for a failed presence check,
because the presence check succeeds and the probe is issued anyway, from the same instruction.

### Proton's DRM handshake is never engaged

**[MEASURED]** The stub dumped `\BaseNamedObjects` and `\BaseNamedObjects\Session\1` every 5 s
for the title's whole lifetime. Across 30 dumps:

- our six objects are present and correctly named;
- **no `STEAM_START_ACK_EVENT*` is ever created** by the wrapper;
- `STEAM_DIPC_CONSUME` is **never signalled**, so `steam_drm_thread` never runs a single iteration.

Proton's DIPC handshake exists for a different wrapper generation. This title does not speak it.

### What this leaves

**[MEASURED]** Everything `steam_helper`'s *presence surface* publishes is now published, and the
title fails at exactly the same instruction as with nothing published at all.

**That is not the same as "we have everything Proton has", and an earlier draft of this section
said so. It was wrong.** Space Marine II works under Proton, so something in that environment does
satisfy this wrapper. `steam_helper` is ~1000 lines and the presence set is a fraction of it. What
is still missing, measured against `proton_11.0`:

| missing | what Proton does | status here |
|---|---|---|
| `steamclient_init_registry` | `steam_helper` loads lsteamclient and calls its private export, which asks the **real client** for the UI language, appid and game language, then writes `HKCU\Software\Valve\Steam\language` and `Apps\<appid>\{Installed=1,Running=1,Updating=0}` | **not implemented at all.** Our bottle has only `ActiveProcess`. **[MEASURED]** but *eliminated for this title*: the wrapper opens exactly four registry keys, all `ActiveProcess`, and never reads `Apps\<appid>`. Still a real gap for other titles — see #98's sibling. |
| `setup_steam_files` | creates `C:\Program Files (x86)\Steam\{config,steamapps}` and writes `libraryfolders.vdf` from `STEAM_COMPAT_LIBRARY_PATHS` | **not implemented.** The bottle has no `Program Files (x86)\Steam` directory at all. Untested. |
| the export table | Proton's `lsteamclient.spec` exports **57 entries by explicit ordinal** (`1 cdecl Breakpad_SteamMiniDumpInit`, … `57 stub hid_write_output_report`) plus the named and overlay sets — matching Valve's real DLL | ours exports **7 names, no ordinals** (#98). Untested against this wrapper. |
| `ValvePlatformMutex` | set to `steam_helper`'s own module path, which under Proton *is* a `steam.exe` under the Steam install dir | we set it, but pointed at the stub's scratchpad path. Wrong shape. |

### The DLL read sits immediately before the probe

**[MEASURED]** An earlier revision noted that the wrapper reads our DLL off disk and called it "not
the fatal step", on the grounds that twelve calls separate the failed `OpenFileMappingA` from
`MessageBoxA`. That measured the wrong gap. The relevant gap is the one *before* the probe, and
there is none:

```
421516.304  CreateFileW(L"C:\shim\steamclient64.dll")  -> 0xbc
421516.305  GetFileSize()                               -> 0x19614D
421516.305  RtlAllocateHeap(0x19614D) / ReadFile()      -> whole DLL into memory
421516.305  HeapFree()                                  -> discarded
421516.305  OpenEventA("Local\SteamStart_SharedMemLock")  <-- next call, zero in between
```

**[SPECULATIVE]** The wrapper reads our entire DLL, frees it, and probes for the shared memory on
the very next call. That is consistent with it inspecting the file — export table, ordinals, PE
headers — and choosing the shared-memory path because of what it found. **This does not contradict
the earlier elimination of #98.** That test added eighteen flat exports as logging stubs and
observed that none was called; a check that parses the export table *on disk* would never call one.
The two tests ask different questions, and only the first has been run.

**[SPECULATIVE]** The alternative reading remains open: a mapping the wrapper *opens* rather than
creates, at a fixed call site, with a fatal dialog three calls after it, is also shaped like a
channel it reads a value out of. That would lean toward the branch ADR 0013 refuses. **Neither is
established.** `0x14551643d` has not been disassembled, and nothing here justifies a `wontfix`.

---

## Resolved: the wrapper requires a Valve RSA signature on `steamclient64.dll`

**[MEASURED]** The `.bind` section of the title's EXE — VMA `0x145513000..0x14554C248`, file offset
`0x050D2400` — is Steam's DRM stub and is **statically present in the file**. It is one function,
`0x1455133D5..0x145516A11`, and it can simply be disassembled:

```sh
objdump -D -j .bind --start-address=0x145516436 "<title>.exe"
```

### 1. `SteamStart_SharedMem*` is a reporting channel, not a check

The protocol, read off `.bind`:

```c
lock = OpenEventA(SYNCHRONIZE, FALSE, "Local\\SteamStart_SharedMemLock");
map  = OpenFileMappingA(FILE_MAP_WRITE, FALSE, "Local\\SteamStart_SharedMemFile");
WaitForSingleObject(lock, 5000);                       // != 0 -> 'G'
shm = MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 0);     // NULL  -> 'H'
if (*(u32*)(shm+0x90) != 2) -> 'I'                     // protocol version
if (*(u32*)(shm+0x94) != 0) -> 'J'                     // slot must be idle
ev = CreateEventA(NULL, FALSE, FALSE, NULL);
*(u32*)(shm+0x98) = GetCurrentProcessId();
*(u32*)(shm+0xA8) = appid;                             // 0x2152DC == 2183900, measured
*(u64*)(shm+0xA0) = (u64)ev;                           // handle valid in the CLIENT's process
*(u32*)(shm+0xB0) = status;                            // the char it is REPORTING
*(u32*)(shm+0x94) = 2;
r = WaitForSingleObject(ev, 5000);                     // wait for Steam to acknowledge
UnmapViewOfFile(shm); CloseHandle(map); CloseHandle(lock); CloseHandle(ev);
                                                       // no payload is ever read back
TerminateProcess(GetCurrentProcess(), status);         // and it ALWAYS exits
```

**[MEASURED]** A stub implementing the server side — create the lock and the mapping, publish
version 2, then duplicate the client's event handle out of its process and signal it — completes the
handshake: `WaitForSingleObject → 0`, and **the `Application load error` dialog stops appearing**.
The title then exits cleanly with code `0x33`.

`0x33` is `'3'`, the first character of `Application load error 3:0000065432`. The routine reports a
status it was *given*; it does not compute one. When it cannot reach Steam it shows the dialog
instead. **Everything this document previously treated as the failure was the error report.**

### 2. The real gate

**[MEASURED]** The status comes from a call at `0x145515C21` into the wrapper's unpacked region.
That region is private memory and in no file — but our own shim is loaded inside the process at the
moment of the decision, so its `CreateInterface` can `VirtualQuery` its caller and dump the region.
Disassembled, the chain is unambiguous:

```
1f34247  GetProcAddress(hSteamClient, "CreateInterface")   ; ok
1f3426d  call  0x1f34040                                   ; <-- validate the providing DLL
1f34272  test  %al,%al
1f34274  jne   ...                                         ; ok -> CreateSteamPipe, ConnectToGlobalUser, ...
1f3427b  mov   $0x33,%al                                   ; FAIL -> status '3'
```

`0x1f34040` resolves `CreateInterface` back to its module (`GetModuleHandleExA` /
`GetModuleFileNameW`), reads that file whole, and hands the bytes to a verifier.

### 3. The verifier wants Valve's signature

**[MEASURED]** The verifier (`0x1f37690` → `0x1f371C0`) is called with three hard-coded
DER-encoded **RSA-1024 public keys** (`06 09 2A 86 48 86 F7 0D 01 01 01` = `rsaEncryption`), and
begins:

```
cmp  $0x200,%rdx           ; file >= 512 bytes
cmpw $0x5A4D,(%rcx)        ; "MZ"
cmpl $0x564C56,0x40(%rdi)  ; "VLV\0" at DOS-header offset 0x40   <-- we fail HERE, returns 2
cmpl $0x1,0x44(%rdi)       ; format version 1                   (returns 4)
mov  0x48(%rdi),%eax       ; signed length, checked against the file size
```

**[MEASURED]** Valve's own `steam_api64.dll`, shipped with this title, carries exactly that block:

```
00000040: 564c 5600 0100 0000 0062 0400  ce1a 4c64 ...   VLV. .... .b..  <sig>
          ^"VLV\0"  ^ver=1   ^len=0x46200  ^128 bytes of RSA-1024 signature
```

File size 298,856; signed length 287,232; signature 128 bytes at `0x4C`. Our shim has Wine's
`Wine builtin DLL` marker at that same offset and no signature anywhere.

**So the wrapper requires `steamclient64.dll` to carry a Valve code signature, verifiable under a
key only Valve holds.**

### How Proton passes it — and it is not by signing `lsteamclient`

**[MEASURED]** An earlier revision of this section claimed Valve must sign their own `lsteamclient`
build. **That is impossible, and wrong.** `winebuild` writes the 32-byte `"Wine builtin DLL"`
signature at file offset **`0x40`** (`tools/winebuild/spec32.c`: it reads the 0x40-byte DOS header,
then writes at the resulting position), and Proton's own `ntdll` reads it back from the same place
(`dlls/ntdll/loader.c:build_module`, `(IMAGE_DOS_HEADER *)module + 1`). Valve's `VLV\0` block
occupies `0x40..0xCB`. **A PE cannot be both a Wine builtin and Valve-signed — they are the same
bytes.** `lsteamclient` is `MODULE = lsteamclient.dll` with a `UNIXLIB`, i.e. an ordinary builtin,
so it would fail this check exactly as ours does.

**[MEASURED]** Proton solves it in the **loader**. `dlls/ntdll/loader.c:build_module` special-cases
any module whose basename is `steamclient`, `steamclient64`, `gameoverlayrenderer` or
`gameoverlayrenderer64`:

```c
if (use_lsteamclient() && (... basename is steamclient64 etc ...) &&
    (lsteamclient || LdrLoadDll(load_path, 0, &lsteamclient_us, &lsteamclient) == STATUS_SUCCESS))
{
    struct steamclient_setup_trampolines_params params = {.src_mod = *module, .tgt_mod = lsteamclient};
    WINE_UNIX_CALL( unix_steamclient_setup_trampolines, &params );
    ...
    wm->ldr.Flags |= LDR_DONT_CALL_DLLMAIN;
}
```

`steamclient_setup_trampolines` (`dlls/ntdll/unix/loader.c`) makes the real DLL's `.text` writable
and, for **every name in its export directory**, overwrites the entry point with a jump to
`lsteamclient`'s export of the same name. `LDR_DONT_CALL_DLLMAIN` means Valve's own code never runs.

**So under Proton the module the DRM inspects is Valve's genuine, signed Windows
`steamclient64.dll`** — right basename, right path, and *the file on disk is untouched*.
`GetModuleHandleExA(FROM_ADDRESS, …)` resolves to it, `GetModuleFileNameW` returns its path, the
wrapper reads that file, and the signature verifies. Truthfully. Nothing is forged: the shim is
reached through trampolines in memory, while the thing being checked on disk is the real article.

**[MEASURED]** The file comes from `$steamdir/legacycompat/steamclient64.dll`, which the `proton`
script copies into the prefix. **macOS Steam does not ship `legacycompat/`** — checked on this
machine; the only `steamclient` there is `Steam.AppBundle/.../steamclient.dylib`.

### What this settles

**Two routes are refused, and one is not.**

Refused: producing an RSA signature over *our* binary that verifies under Valve's key — forging a
signature, cryptographically infeasible and named by
[ADR 0013](../adr/0013-a-presence-stub-may-represent-the-native-client-in-the-bottle.md). Also
refused: patching the check out of the title's own `.bind`, which is defeating DRM on someone else's
binary.

**Not refused: Proton's own route.** Ship no signature, alter no file, and let the wrapper inspect
Valve's genuine signed `steamclient64.dll` — because it genuinely is on disk — while our shim is
reached through trampolines written into that module's exports in memory. The wrapper's question is
*"is the Steam client DLL on disk the real Valve one?"*, and the honest answer is yes. This is the
same shape as the overlay (ADR 0003), where we already load Valve's own renderer rather than
imitating it.

That route is **not blocked by cryptography**. It is blocked by two concrete things:

1. **Provisioning.** It needs Valve's genuine Windows `steamclient64.dll`. macOS Steam does not
   ship one, so it would have to come from a Linux or Windows Steam install — a distribution
   question, not a technical one, and the first thing to settle because it gates the rest.
2. **The trampoline site.** Proton does the load-and-rewrite inside its own `ntdll`. We defer the
   emulator to CrossOver and cannot patch its loader. We do, however, already create the title
   suspended and rewrite its imports before its own code runs (ADR 0003, `src/overlay-inject/`),
   which is a plausible place to do the same work from outside the loader. Unproven.

**The scope is one wrapper, not the project.** Nothing here touches titles that ship without the
Steam DRM wrapper; the shim, the overlay and the compat tool are unaffected.

### Eliminated along the way

**[MEASURED]** Each of these was built and run, and none changed the outcome:

| candidate | result |
|---|---|
| `ActiveProcess\pid` | read, `OpenProcess`ed, liveness-checked — no change |
| Proton's full presence set + parent topology | no change; probe issued from the same instruction |
| Proton's DIPC handshake | never engaged — no `STEAM_START_ACK_EVENT`, consume never signalled |
| the export table (57 ordinals + named set, matching `lsteamclient.spec`) | no change; no flat export ever called |
| `steamclient_init_registry` (`Apps\<appid>\Running` etc.) | the wrapper opens four registry keys, all `ActiveProcess` — never reads it |
| `CreateInterface`'s `returnCode` | passed as `NULL` by the wrapper; cannot matter |
| `GetProcAddress(0, "Steam_ReleaseThreadLocalMemory")` | result stored and never checked; benign |

---

## Solved: Valve's real signed DLL, trampolined into our shim

**[MEASURED]** A genuine Windows `steamclient64.dll` was already on this machine — in the
`Steam-2` CrossOver bottle's Windows Steam install, 26,222,744 bytes, `VLV\0` / version 1 /
signed length `0x018FF400` / 128-byte signature at `0x4C`, exporting `CreateInterface`.

Copied to `C:\vsteam\` with its two non-system dependencies (`tier0_s64.dll`, `vstdlib_s64.dll`),
with `SteamClientDll64` pointed at it and **our builtin moved aside** — the builtin wins on
basename from `WINEDLLPATH` otherwise, and `WINEDLLOVERRIDES` does not survive CrossOver's front
door:

| | our shim | Valve's signed DLL |
|---|---|---|
| `SteamStart_SharedMemLock` probes | 1 | **0** |
| `Application load error` dialog | yes (unless answered) | **never** |
| status reported | `'3'` | **never reaches the report** |

**The signature check passes.** `0x1f34040` returns true, the wrapper proceeds to the vtable stage,
and the DRM gate is behind it.

**[MEASURED]** What fails next is Valve's own client code, not the DRM:

```
GetClassInfoExW(L"SteamWinsockInitFakeClass_1")
OpenEventA("Steam3Master_SharedMemLock")
OpenFileMappingA("Steam3Master_SharedMemFile")   -> NULL
"src\common\processpipe_any.cpp (248) : Assertion Failed: OpenFileMapping returned NULL (errno=2)"
```

That is exactly the code Proton suppresses with `LDR_DONT_CALL_DLLMAIN` and replaces with
trampolines. **The route is validated end to end: the gate is passable without forging anything,
and what remains is redirecting the signed DLL's exports to our shim.**

### A trampoline site that needs no injection

**[MEASURED]** The wrapper loads the DLL with `LoadLibraryExW(path, 0, LOAD_WITH_ALTERED_SEARCH_PATH)`,
so **the DLL's own directory is searched first for its dependencies — and we own that directory.**
Its only non-system imports are Valve's `tier0_s64.dll` and `vstdlib_s64.dll`. Initialisation order,
measured:

```
426596.203  tier0_s64.dll      PROCESS_ATTACH  -> 1
426596.220  vstdlib_s64.dll    PROCESS_ATTACH  -> 1
426596.220  steamclient64.dll  PROCESS_ATTACH  ... 1.2 s ...
426597.372                                     -> 1
426597.378  GetProcAddress(steamclient64, "CreateInterface")
```

A shim `tier0_s64.dll` planted beside the signed DLL gets its `DllMain` **1.2 s before**
`steamclient64.dll`'s own entry point finishes and before the wrapper asks for `CreateInterface` —
room to rewrite the exports and neuter the entry point.

**[MEASURED]** The hook was then built and run, and it works. A shadow `tier0_s64.dll` — 165 no-op
stubs matching what `steamclient64.dll` imports, plus a probing `DllMain` — planted beside Valve's
signed DLL with Valve's real `tier0` absent:

```
tier0 shim DllMain: PROCESS_ATTACH, self=00006FFFF9940000
GetModuleHandleA(steamclient64.dll) = 00006FFFF99B0000
  entrypoint RVA 0xf7fe24, export dir RVA 0x16b7fb0
  exports: 41 names, first = Breakpad_SteamMiniDumpInit
  VirtualProtect(.text RW) = 1 (err 183, old 0x20)
```

Four things at once: **our** shadow was loaded rather than Valve's; it ran while `steamclient64.dll`
was **already mapped**; its export directory is readable (**41 names**, matching Valve's real export
count — the same 41 that #98 measures against); and its `.text` is **writable**, so the exports can
be rewritten and the entry point (`RVA 0xf7fe24`) neutered.

### It works: Space Marine II launches

**[MEASURED]** The assembly was then built and the title **launched, reached shader
pre-compilation, and stayed running** — the first DRM-wrapped title this project has started.

The working arrangement:

| piece | what it is |
|---|---|
| `C:\vsteam\valve_steamclient64.dll` | **Valve's genuine signed DLL**, byte-for-byte. `SteamClientDll64` points here. Renamed because a builtin of ours with the same basename would win the lookup and be rejected as unsigned. |
| `C:\vsteam\tier0_s64.dll` | ours. 165 stub exports + a `DllMain` that installs the trampolines. |
| `C:\vsteam\vstdlib_s64.dll` | ours. 134 stub exports, inert `DllMain`. |
| `C:\shim\lsteamclient.dll` + `.so` | our shim, **built under that name** (see below). |

On load, our `tier0` shadow rewrites each of the signed DLL's 41 exports to a jump — 7 into our
shim's forwarders, 34 into a stub returning zero — and overwrites its entry point with
`mov eax,1 ; ret`. **No Valve client code ever executes.** The forwarders bind lazily, so nothing
calls `LoadLibrary` under the loader lock.

```
tier0 shadow attached, self=00006FFFF9940000
exports: 41 total, 7 -> shim, 34 -> dead
entry point 00006FFFFA92FE24 neutered
lazy LoadLibraryA(C:\shim\lsteamclient.dll) = 00006FFFF97A0000
  bind CreateInterface -> 00006FFFF983A0B0
```

and then, past the DRM, the title's own Steamworks init against the native macOS client:

```
shim: CreateInterface("SteamClient017")   <- the DRM
shim: CreateInterface("SteamClient014")   <- the DRM
shim: CreateInterface("SteamClient017")   <- the TITLE
shim: CreateInterface("SteamClient020")
shim: acquire("SteamUser023")  acquire("SteamUtils010")  acquire("SteamInput006")
shim: GetAppID() -> 2183900
shim: GetCurrentGameLanguage() -> english
shim: acquire("STEAMUGC_INTERFACE_VERSION017")  acquire("SteamFriends017")
```

**[MEASURED] The shim must be BUILT as `lsteamclient`, not renamed.** A byte copy fails:
`find_builtin_dll` derives the builtin name from the PE's **internal** name (its export directory
`Name`), not the file basename, so a copied `steamclient64.dll` sends ntdll hunting for
`steamclient64.so` and the unixlib never binds — `seam unavailable (not Wine / no .so)`, with
`CreateInterface` answering from a PE with no unix half. Rebuild with `-o lsteamclient.dll`.
The rename is unavoidable regardless: two modules cannot share a basename in one process, and
Valve's signed DLL must keep `steamclient64.dll`.

**[MEASURED]** Both support libraries must be shadowed. With our stubbed `tier0` and Valve's **real**
`vstdlib_s64.dll`, the title dies before `CreateInterface` — the real `vstdlib` initialises against
stub `tier0`. Neither is ever called once the entry point is neutered, so both can be inert.
Its merit is that it is **in-process, at loader time**: no suspended process, no `CreateRemoteThread`,
no rewriting of the *title's* imports — so it does not carry the overlay injector's anti-tamper
exposure (ADR 0003, and the AoE IV / Aegis case). That matters here because the motivating title
ships EasyAntiCheat itself.

---

## How to re-run this

```sh
# 1. The PE-side view: which interfaces, which slots, what was returned.
SHIM_PE_LOG=/tmp/sm2.pe.log wine --bottle steam-shim "<title>.exe"

# 2. The full API trace. CrossOver's own redirect -- the game is a detached GUI
#    child, so plain stderr redirection captures nothing.
wine --bottle steam-shim --cx-log /tmp/relay.log --debugmsg +relay "<title>.exe"
grep -n "SteamStart\|MessageBox" /tmp/relay.log
```

The relay log for this title reached 180 MB in ~150 s and the decisive window is ~350 lines wide,
anchored on the `GetProcAddress(..., "CreateInterface")` whose `ret=` lands in the unpacked region.
Filter by thread id prefix first — the log is multi-threaded and sequences interleave.

To reproduce the presence-set run, a Windows stub publishing Proton's full surface must be alive in
the same bottle before the title starts. It is ~230 lines of C against `steam_helper/steam.c`, built
with the same mingw as the shim; it was deliberately **not** kept, because it answered its question
(see the section above) and shipping it would have implied it worked.

Both are throwaway measurements, and both leave state that corrupts the next run:

```sh
pkill -f "Retail.exe"; pkill -f "<your stub>.exe"
wine --bottle steam-shim reg delete \
    "HKCU\\Software\\Valve\\Steam\\ActiveProcess" /v pid /f
```

`ActiveProcess\pid` **does not exist** in the clean bottle. That is the baseline every trace above
was taken against; restore it.

---

## Wrong turns

Three, all of them mine, and each cost a run.

**1. "It's EasyAntiCheat."** The title ships behind `start_protected_game.exe`, and the widely
circulated fix is to edit `Warhammer 40000 Space Marine 2.ini` to point `ApplicationPath` at
`client_pc\root\bin\pc\Warhammer 40000 Space Marine 2 - Retail.exe`. That edit **works** — the PE
module map confirms the retail EXE is the loaded main module — and it changes nothing, because the
DRM wrapper is on the retail EXE too. EAC governs multiplayer, not this dialog.

**2. "The wrapper fails before it reads the ini."** Asserted on the strength of `grep` finding
`Application load error` in no file in the game directory. That grep was worthless: the string is
UTF-16 in the binary, and an ASCII `grep -r` cannot see it. The relay trace later showed the
string being passed to `MessageBoxA` from unpacked memory, so it is in no file at all.

**3. "It checks our DLL's signature."** The most confident of the three, argued from `WINTRUST.dll`
and `CRYPT32.dll` being in the module list and our shim being an unsigned mingw PE with 7 exports
against Valve's signed 41. **Refuted by measurement**: the wrapper calls no signature API, and
`+wintrust,+chain,+crypt` traced nothing. Those two modules are loaded by something else in the
process. The DLL *is* read from disk — which is what made the theory survive as long as it did —
but the branch is on the shared memory, not on the bytes.

A fourth was proposed and killed before it cost anything: that the wrapper wanted one of the 34
flat exports we do not implement (`Steam_CreateSteamPipe` and friends). Eighteen were added as
logging stubs; none was ever called. That gap is real and worth fixing on its own merits, but it
is not this bug.
