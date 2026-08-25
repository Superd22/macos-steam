/* inputprobe — the #28 oracle: what reaches the TITLE while the overlay is up?
 *
 * #28 asks whether the game keeps receiving input while the Steam overlay is
 * open. On Windows and in Proton the answer is "no, by design": lsteamclient
 * raises GameOverlayActivated_t and fires a keybd_event, and Valve's Wine fork
 * consumes the other end in winex11.drv, dinput, hidclass.sys and xinput1_3.
 * CrossOver's Wine has none of those consumers, so the gate's second half is
 * simply absent here and nobody has measured the cost.
 *
 * Impressions are not a measurement. This probe is d3dprobe (a real D3D11 swap
 * chain, therefore a real overlay target — #26) with every input channel the
 * ticket names wired to a timestamped log:
 *
 *     keyboard   WM_KEY* / WM_CHAR from the message queue
 *     mouse      WM_MOUSEMOVE / clicks, plus the cursor's screen position
 *                and the ShowCursor counter (titles that hide the cursor are
 *                the interesting case)
 *     xinput     XInputGetState packet numbers, buttons, sticks, triggers
 *     dinput8    the same pad through DirectInput's joystick path
 *
 * Every line is stamped from a monotonic clock and tagged with the overlay's
 * believed state, so "did WASD reach the game while I typed in the overlay" is
 * answered by reading the log, not by watching a screen.
 *
 * Marking the overlay's open/close in the log without trusting our own
 * keystroke handling (the very thing under test) is done by watching Shift+Tab
 * at the raw GetAsyncKeyState level and by SHIM_INPUTPROBE_MARK — see below.
 *
 * Build: build.sh (both bitnesses). Run inside the bottle exactly like
 * d3dprobe, with the overlay armed. See README.md.
 */
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ logging */
/* File, not a console: a console exe attaches to conhost before main, that
 * path calls into USER, and USER demand-loads winemac.so — so a printf probe
 * has lost the overlay race before its first line of code (#26). */
static FILE *g_out;
static LARGE_INTEGER g_freq, g_t0;

static double now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

/* What we BELIEVE the overlay state is. Believed, not known: we have no
 * callback from the renderer, so this is inferred from Shift+Tab and from the
 * marker file. It is logged on every line precisely so a wrong belief is
 * visible as a contradiction rather than silently colouring the result. */
static int g_overlay_believed_up;

static void plog(const char *fmt, ...)
{
    va_list ap;
    if (!g_out) {
        g_out = fopen("C:\\inputprobe.log", "w");
        if (!g_out) return;
        setvbuf(g_out, NULL, _IONBF, 0);
    }
    fprintf(g_out, "[%9.1fms ov=%d] ", now_ms(), g_overlay_believed_up);
    va_start(ap, fmt);
    vfprintf(g_out, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------- renderer pull-in */
typedef void *(__cdecl *CreateInterfaceFn)(const char *, int *);

/* Identical to d3dprobe's: LoadLibrary the shim PE and touch the seam, which
 * makes ntdll dlopen the unixlib, whose constructor dlopens Valve's renderer.
 * Must happen before the first USER call. SHIM_NO_SELF_PULL=1 disables it so
 * the probe is an honest target for the injector instead (#25). */
static void pull_in_renderer(void)
{
#ifdef _WIN64
    const char *dll = "C:\\shim\\steamclient64.dll";
#else
    const char *dll = "C:\\shim\\steamclient.dll";
#endif
    HMODULE m;
    CreateInterfaceFn ci;

    if (GetEnvironmentVariableA("SHIM_NO_SELF_PULL", NULL, 0) > 0) {
        plog("self-pull disabled — renderer must arrive by injection\n");
        return;
    }
    m = LoadLibraryA(dll);
    plog("LoadLibrary(%s) -> %p\n", dll, (void *)m);
    if (!m) { plog("  (no shim PE — renderer will NOT be loaded)\n"); return; }
    ci = (CreateInterfaceFn)GetProcAddress(m, "CreateInterface");
    if (ci) { int rc = 0; ci("inputprobe_forces_the_unixlib_to_load", &rc); }
    plog("renderer pulled in; check /tmp/gameoverlayrenderer.%lu.log for 'Hooking'\n",
         (unsigned long)GetCurrentProcessId());
}

/* --------------------------------------------------------------- xinput */
/* Bound by name, not by import library: a static import to xinput1_4 would run
 * its DllMain before main and could touch USER, which is the one thing this
 * probe must not do before the renderer is in. */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger;
                 SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XI_GAMEPAD;
typedef struct { DWORD dwPacketNumber; XI_GAMEPAD Gamepad; } XI_STATE;
typedef DWORD (WINAPI *pXInputGetState)(DWORD, XI_STATE *);
typedef void  (WINAPI *pXInputEnable)(BOOL);

static pXInputGetState g_xi_get;
static pXInputEnable   g_xi_enable;
static const char     *g_xi_which = "none";

static void xinput_open(void)
{
    /* Newest first: whichever CrossOver actually resolves is the one a title
     * would get. The name is logged so the measurement says which stack it
     * exercised rather than leaving it to be assumed. */
    static const char *cands[] = { "xinput1_4.dll", "xinput1_3.dll",
                                   "xinput9_1_0.dll", "xinput1_2.dll",
                                   "xinput1_1.dll", NULL };
    int i;
    for (i = 0; cands[i]; i++) {
        HMODULE m = LoadLibraryA(cands[i]);
        if (!m) continue;
        g_xi_get = (pXInputGetState)GetProcAddress(m, "XInputGetState");
        if (!g_xi_get) continue;
        g_xi_enable = (pXInputEnable)GetProcAddress(m, "XInputEnable");
        g_xi_which = cands[i];
        plog("xinput: %s XInputGetState=%p XInputEnable=%p\n",
             cands[i], (void *)g_xi_get, (void *)g_xi_enable);
        return;
    }
    plog("xinput: NO usable xinput dll found\n");
}

/* Log only changes — a 60Hz dump of an idle pad would bury the signal. The
 * packet number is XInput's own "something changed" counter, and the deadzone
 * on the sticks keeps a resting analogue stick from chattering. */
static void xinput_poll(void)
{
    static DWORD last_packet[4];
    static int   seen[4];
    DWORD pad;

    if (!g_xi_get) return;
    for (pad = 0; pad < 4; pad++) {
        XI_STATE st;
        DWORD rc = g_xi_get(pad, &st);
        if (rc != ERROR_SUCCESS) {
            if (seen[pad]) { plog("xinput pad%lu DISCONNECTED (rc=%lu)\n", (unsigned long)pad, (unsigned long)rc); seen[pad] = 0; }
            continue;
        }
        if (!seen[pad]) { plog("xinput pad%lu connected\n", (unsigned long)pad); seen[pad] = 1; last_packet[pad] = 0; }
        if (st.dwPacketNumber == last_packet[pad]) continue;
        last_packet[pad] = st.dwPacketNumber;
        plog("XINPUT pad%lu packet=%lu buttons=0x%04x LT=%3u RT=%3u L=(%6d,%6d) R=(%6d,%6d)\n",
             (unsigned long)pad, (unsigned long)st.dwPacketNumber,
             (unsigned)st.Gamepad.wButtons,
             (unsigned)st.Gamepad.bLeftTrigger, (unsigned)st.Gamepad.bRightTrigger,
             st.Gamepad.sThumbLX, st.Gamepad.sThumbLY,
             st.Gamepad.sThumbRX, st.Gamepad.sThumbRY);
    }
}

/* -------------------------------------------------------------- dinput8 */
/* dinput is the other half of #28's gamepad question, and it is a genuinely
 * different path through Wine — dinput8.dll over hidclass, rather than
 * winexinput. A pad can leak through one and be gated on the other, so both
 * are measured. Bound dynamically for the same reason as xinput. */
#include <dinput.h>

typedef HRESULT (WINAPI *pDirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
static LPDIRECTINPUT8      g_di;
static LPDIRECTINPUTDEVICE8 g_di_pad;

static BOOL CALLBACK di_enum(const DIDEVICEINSTANCEA *inst, void *ctx)
{
    (void)ctx;
    plog("dinput: device \"%s\" (product \"%s\") type=0x%08lx\n",
         inst->tszInstanceName, inst->tszProductName, (unsigned long)inst->dwDevType);
    if (!g_di_pad) {
        if (FAILED(g_di->lpVtbl->CreateDevice(g_di, &inst->guidInstance, &g_di_pad, NULL)))
            g_di_pad = NULL;
    }
    return DIENUM_CONTINUE;
}

static void dinput_open(HWND hwnd)
{
    HMODULE m = LoadLibraryA("dinput8.dll");
    pDirectInput8Create create;
    HRESULT hr;

    if (!m) { plog("dinput: dinput8.dll not loadable\n"); return; }
    create = (pDirectInput8Create)GetProcAddress(m, "DirectInput8Create");
    if (!create) { plog("dinput: no DirectInput8Create\n"); return; }
    hr = create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, (void **)&g_di, NULL);
    if (FAILED(hr) || !g_di) { plog("dinput: DirectInput8Create hr=0x%08lx\n", (unsigned long)hr); return; }

    g_di->lpVtbl->EnumDevices(g_di, DI8DEVCLASS_GAMECTRL, di_enum, NULL, DIEDFL_ATTACHEDONLY);
    if (!g_di_pad) { plog("dinput: no game controller enumerated\n"); return; }

    g_di_pad->lpVtbl->SetDataFormat(g_di_pad, &c_dfDIJoystick2);
    /* NONEXCLUSIVE|FOREGROUND is what an ordinary title asks for, so the
     * cooperative level matches what we are trying to characterise. FOREGROUND
     * also means acquisition is expected to LAPSE when the overlay takes focus
     * — if it does, that is itself a finding, and the reacquire below records
     * how quickly the title would get input back. */
    hr = g_di_pad->lpVtbl->SetCooperativeLevel(g_di_pad, hwnd, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
    plog("dinput: SetCooperativeLevel hr=0x%08lx\n", (unsigned long)hr);
    hr = g_di_pad->lpVtbl->Acquire(g_di_pad);
    plog("dinput: Acquire hr=0x%08lx\n", (unsigned long)hr);
}

static void dinput_poll(void)
{
    static DIJOYSTATE2 prev;
    static int have_prev, was_lost;
    DIJOYSTATE2 js;
    HRESULT hr;

    if (!g_di_pad) return;
    g_di_pad->lpVtbl->Poll(g_di_pad);
    hr = g_di_pad->lpVtbl->GetDeviceState(g_di_pad, sizeof(js), &js);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        if (!was_lost) { plog("DINPUT input LOST (hr=0x%08lx) — reacquiring\n", (unsigned long)hr); was_lost = 1; }
        g_di_pad->lpVtbl->Acquire(g_di_pad);
        return;
    }
    if (FAILED(hr)) return;
    if (was_lost) { plog("DINPUT reacquired\n"); was_lost = 0; }

    /* Only report real motion. Axes rest near centre (32767) and jitter, so a
     * threshold keeps the log to actual stick movement and button edges. */
    if (have_prev) {
        long dx = js.lX - prev.lX, dy = js.lY - prev.lY;
        long rx = js.lRx - prev.lRx, ry = js.lRy - prev.lRy;
        int btn_changed = memcmp(js.rgbButtons, prev.rgbButtons, sizeof(js.rgbButtons)) != 0;
        int pov_changed = js.rgdwPOV[0] != prev.rgdwPOV[0];
        long mag = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy)
                 + (rx < 0 ? -rx : rx) + (ry < 0 ? -ry : ry);
        if (btn_changed || pov_changed || mag > 2000) {
            int i, first = -1;
            for (i = 0; i < 32; i++) if (js.rgbButtons[i] & 0x80) { first = i; break; }
            plog("DINPUT L=(%5ld,%5ld) R=(%5ld,%5ld) pov=%lu btn0down=%d\n",
                 js.lX, js.lY, js.lRx, js.lRy,
                 (unsigned long)js.rgdwPOV[0], first);
        }
    }
    prev = js; have_prev = 1;
}

/* ------------------------------------------------- overlay state tracking */
/* Shift+Tab is read RAW, through GetAsyncKeyState, deliberately: the message
 * queue is the thing under test, so inferring the overlay's state from it
 * would make the instrument depend on its own subject. GetAsyncKeyState reads
 * the driver-level key table instead.
 *
 * That the raw table sees Shift+Tab AT ALL while the overlay is up is itself a
 * result — it means the keystroke was not consumed before Wine's input layer. */
static void track_overlay_key(void)
{
    static int was_down;
    int down = (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_TAB) & 0x8000);
    if (down && !was_down) {
        g_overlay_believed_up = !g_overlay_believed_up;
        plog("=== Shift+Tab seen RAW by the title — overlay believed %s ===\n",
             g_overlay_believed_up ? "UP" : "DOWN");
    }
    was_down = down;
}

/* A file-driven marker, so the operator can stamp the log from outside the
 * process without typing into it. Writing a line into C:\inputprobe.mark (from
 * the mac side: <bottle>/drive_c/inputprobe.mark) both records the note and
 * sets the believed overlay state, which is how a run gets ground truth that
 * does not depend on our own Shift+Tab inference being right. */
static void poll_marker(void)
{
    static FILETIME last;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    char buf[256];
    HANDLE h;
    DWORD got = 0;

    if (!GetFileAttributesExA("C:\\inputprobe.mark", GetFileExInfoStandard, &fad)) return;
    if (fad.ftLastWriteTime.dwLowDateTime == last.dwLowDateTime &&
        fad.ftLastWriteTime.dwHighDateTime == last.dwHighDateTime) return;
    last = fad.ftLastWriteTime;

    h = CreateFileA("C:\\inputprobe.mark", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (ReadFile(h, buf, sizeof(buf) - 1, &got, NULL)) {
        buf[got] = 0;
        while (got && (buf[got - 1] == '\n' || buf[got - 1] == '\r')) buf[--got] = 0;
        if (!strncmp(buf, "up", 2))   g_overlay_believed_up = 1;
        if (!strncmp(buf, "down", 4)) g_overlay_believed_up = 0;
        plog("=== MARK: %s ===\n", buf);
    }
    CloseHandle(h);
}

/* ------------------------------------------------------------ mouse state */
/* The cursor is the half of #28 that a title can leave in a genuinely bad
 * state: a game that hides and warps the pointer needs it back, visible and
 * where it was, when the overlay closes. ShowCursor(TRUE) returns the display
 * counter without changing it if we immediately undo it, which is how the
 * counter is sampled without perturbing what we are measuring. */
static void mouse_poll(void)
{
    static POINT prev;
    static int have_prev, prev_count = 999;
    POINT p;
    int count;

    if (!GetCursorPos(&p)) return;
    count = ShowCursor(TRUE);
    ShowCursor(FALSE);

    if (count != prev_count) {
        plog("CURSOR display counter -> %d (%s)\n", count, count >= 0 ? "visible" : "hidden");
        prev_count = count;
    }
    if (have_prev && (p.x != prev.x || p.y != prev.y)) {
        plog("CURSOR screen pos (%ld,%ld)\n", (long)p.x, (long)p.y);
    }
    prev = p; have_prev = 1;
}

/* ----------------------------------------------------------------- window */
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_DESTROY: PostQuitMessage(0); return 0;

    /* The headline question: does a keystroke typed INTO THE OVERLAY also
     * arrive here, in the title's own message queue? Every key event is
     * logged with the believed overlay state beside it. */
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        plog("KEY   down vk=0x%02x '%c' repeat=%d\n", (unsigned)w,
             (w >= 32 && w < 127) ? (char)w : '.', (int)(l & 0xffff));
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        plog("KEY   up   vk=0x%02x '%c'\n", (unsigned)w,
             (w >= 32 && w < 127) ? (char)w : '.');
        return 0;
    case WM_CHAR:
        plog("CHAR  '%c' (0x%02x)\n", (w >= 32 && w < 127) ? (char)w : '.', (unsigned)w);
        return 0;

    case WM_LBUTTONDOWN: plog("MOUSE Ldown at (%d,%d)\n", LOWORD(l), HIWORD(l)); return 0;
    case WM_LBUTTONUP:   plog("MOUSE Lup   at (%d,%d)\n", LOWORD(l), HIWORD(l)); return 0;
    case WM_RBUTTONDOWN: plog("MOUSE Rdown at (%d,%d)\n", LOWORD(l), HIWORD(l)); return 0;
    case WM_MOUSEMOVE: {
        /* Rate-limited: a move burst is thousands of messages and the question
         * is only whether ANY arrive while the overlay is up. */
        static double last;
        double t = now_ms();
        if (t - last > 250.0) { plog("MOUSE move to (%d,%d)\n", LOWORD(l), HIWORD(l)); last = t; }
        return 0;
    }

    /* Focus is the mechanism a well-behaved title would use to pause itself,
     * and whether the overlay takes it here is a load-bearing observation:
     * if the title never loses focus, nothing in its own logic will fire. */
    case WM_ACTIVATE:
        plog("WINDOW WM_ACTIVATE %s\n", LOWORD(w) ? "ACTIVATED" : "DEACTIVATED");
        return 0;
    case WM_ACTIVATEAPP:
        plog("WINDOW WM_ACTIVATEAPP %s\n", w ? "app active" : "app INACTIVE");
        return 0;
    case WM_KILLFOCUS: plog("WINDOW WM_KILLFOCUS\n"); return 0;
    case WM_SETFOCUS:  plog("WINDOW WM_SETFOCUS\n");  return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc = {0};
    HWND hwnd;
    DXGI_SWAP_CHAIN_DESC sd = {0};
    HMODULE d3d11;
    IDXGISwapChain *sc = NULL; ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL;
    ID3D11Texture2D *back = NULL; ID3D11RenderTargetView *rtv = NULL;
    D3D_FEATURE_LEVEL got; HRESULT hr; int frames = 0;

    typedef HRESULT (WINAPI *pD3D11CreateDeviceAndSwapChain)(
        IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
        UINT, UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **,
        D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
    pD3D11CreateDeviceAndSwapChain create;

    (void)inst; (void)prev; (void)cmd; (void)show;

    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);

    pull_in_renderer();                    /* BEFORE the first USER call */

    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "inputprobe";
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "inputprobe", "inputprobe — #28 input parity",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           100, 100, 800, 600, NULL, NULL, wc.hInstance, NULL);
    plog("hwnd=%p pid=%lu\n", (void *)hwnd, (unsigned long)GetCurrentProcessId());
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    sd.BufferCount = 2;
    sd.BufferDesc.Width = 800;
    sd.BufferDesc.Height = 600;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    d3d11 = LoadLibraryA("d3d11.dll");
    create = d3d11 ? (pD3D11CreateDeviceAndSwapChain)
                     GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain") : NULL;
    if (!create) { plog("no D3D11 entry point\n"); return 2; }
    hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                &sd, &sc, &dev, &got, &ctx);
    plog("D3D11CreateDeviceAndSwapChain hr=0x%08lx featurelevel=0x%x\n",
         (unsigned long)hr, (unsigned)got);
    if (FAILED(hr)) { plog("no D3D11 device — not a valid overlay target\n"); return 2; }

    sc->lpVtbl->GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&back);
    dev->lpVtbl->CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv);

    /* Input opened AFTER the renderer and the device, the way a title would:
     * nothing here is on the overlay's critical path. */
    xinput_open();
    if (g_xi_enable) g_xi_enable(TRUE);
    dinput_open(hwnd);

    plog("--- ready. press Shift+Tab, then exercise keyboard / mouse / pad ---\n");

    for (;;) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        track_overlay_key();
        poll_marker();
        xinput_poll();
        dinput_poll();
        mouse_poll();

        {   /* A colour cycle, so a frozen frame is obvious from a live one —
             * which also tells us whether the title kept rendering under the
             * overlay, the other half of "is it paused". */
            float t = (float)(frames % 240) / 240.0f;
            float rgb[4] = { 0.1f + 0.3f * t, 0.2f, 0.5f - 0.3f * t, 1.0f };
            ctx->lpVtbl->ClearRenderTargetView(ctx, rtv, rgb);
        }
        sc->lpVtbl->Present(sc, 1, 0);
        if (++frames % 600 == 0) plog("still rendering, frames=%d\n", frames);
        if (frames > 60 * 60 * 20) break;
    }
done:
    plog("exiting after %d frames\n", frames);
    return 0;
}
