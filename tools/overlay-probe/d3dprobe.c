/* d3dprobe — S-4 (#26): does Valve's overlay see D3DMetal's frames?
 *
 * metalprobe answered the question for a plain CAMetalLayer. A Windows title
 * renders through Direct3D, which CrossOver translates to Metal (D3DMetal), and
 * the study's claim is that Valve's swizzles sit BELOW that translation — it
 * hooks Apple's CAMetalDrawable / MTLCommandBuffer, which D3DMetal must bottom
 * out into. Nobody had run it.
 *
 * No sideloading needed: we own this exe, so we can pull the renderer in from
 * the top of main. That is early enough because winemac.so is demand-loaded
 * (Addendum 2 B7) — it arrives on the first USER call, and we make none until
 * after the renderer is in. Load order here mirrors what #25's injector will do
 * for real titles.
 *
 *   1. LoadLibrary our shim PE and call CreateInterface, which forces ntdll to
 *      dlopen steamclient64.so, whose constructor dlopens the renderer.
 *   2. THEN create the window and the D3D11 device, and present frames.
 *
 * Build: see build.sh (needs mingw + WINEDLLPATH pointing at the shim dir so
 * the unixlib is found beside the PE). Run with Steam up, SHIM_OVERLAY=1,
 * SteamOverlayGameId set to a real appid, SteamNoOverlayUIDrawing UNSET, and
 * STEAM_OVERLAY_LOGGING=1 — then press Shift+Tab.
 */
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdarg.h>

/* Logging goes to a file, not a console. A console exe attaches to conhost
 * before main, that path calls into USER, and USER demand-loads winemac.so —
 * so a printf-based probe has already lost the race before its first line of
 * code. Measured: the console build found winemac.so present at attach. */
static FILE *g_out;
static void plog(const char *fmt, ...)
{
    va_list ap;
    if (!g_out) { g_out = fopen("C:\\d3dprobe.log", "w"); if (!g_out) return;
                  setvbuf(g_out, NULL, _IONBF, 0); }
    va_start(ap, fmt); vfprintf(g_out, fmt, ap); va_end(ap);
}
#define printf plog

typedef void *(__cdecl *CreateInterfaceFn)(const char *, int *);

/* Step 1: get the renderer into the process before we touch USER. */
static void pull_in_renderer(void)
{
    const char *dll = "C:\\shim\\steamclient64.dll";
    /* SHIM_NO_SELF_PULL=1 makes this an honest target for #25's injector: the
     * probe then does nothing to help itself, so a Hooking line in the renderer
     * log is the injector's doing and nobody else's. */
    if (GetEnvironmentVariableA("SHIM_NO_SELF_PULL", NULL, 0) > 0) {
        printf("self-pull disabled — renderer must arrive by injection\n");
        return;
    }
    HMODULE m = LoadLibraryA(dll);
    printf("LoadLibrary(%s) -> %p\n", dll, (void *)m);
    if (!m) { printf("  (no shim PE — renderer will NOT be loaded)\n"); return; }

    /* Calling any seam entry point is what makes the PE bind its unixlib, and
     * binding is what makes ntdll dlopen steamclient64.so. The interface name is
     * deliberately bogus: we want the load, not the interface. */
    CreateInterfaceFn ci = (CreateInterfaceFn)GetProcAddress(m, "CreateInterface");
    printf("CreateInterface=%p\n", (void *)ci);
    if (ci) { int rc = 0; ci("d3dprobe_forces_the_unixlib_to_load", &rc); }
    printf("renderer pulled in; check /tmp/gameoverlayrenderer.%lu.log for 'Hooking'\n",
           (unsigned long)GetCurrentProcessId());
    fflush(stdout);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)inst; (void)prev; (void)cmd; (void)show;
    pull_in_renderer();                    /* BEFORE the first USER call */

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "d3dprobe";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "d3dprobe", "d3dprobe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                100, 100, 640, 480, NULL, NULL, wc.hInstance, NULL);
    printf("hwnd=%p\n", (void *)hwnd);

    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 640;
    sd.BufferDesc.Height = 480;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    /* d3d11 is loaded HERE, by hand, rather than imported — a static import runs
     * its DllMain before main, i.e. before we can pull the renderer in. */
    typedef HRESULT (WINAPI *pD3D11CreateDeviceAndSwapChain)(
        IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
        UINT, UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **,
        D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    pD3D11CreateDeviceAndSwapChain create = d3d11
        ? (pD3D11CreateDeviceAndSwapChain)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")
        : NULL;
    printf("d3d11.dll=%p create=%p\n", (void *)d3d11, (void *)create);
    if (!create) { printf("no D3D11 entry point\n"); return 2; }

    IDXGISwapChain *sc = NULL; ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL got;
    HRESULT hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                        NULL, 0, D3D11_SDK_VERSION,
                        &sd, &sc, &dev, &got, &ctx);
    printf("D3D11CreateDeviceAndSwapChain hr=0x%08lx featurelevel=0x%x\n",
           (unsigned long)hr, (unsigned)got);
    if (FAILED(hr)) { printf("no D3D11 device — cannot answer S-4\n"); return 2; }

    ID3D11Texture2D *back = NULL;
    sc->lpVtbl->GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&back);
    ID3D11RenderTargetView *rtv = NULL;
    dev->lpVtbl->CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv);
    printf("rendering — press Shift+Tab\n");
    fflush(stdout);

    int frames = 0;
    for (;;) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        /* A colour cycle, so a frozen frame is obvious from a live one. */
        float t = (float)(frames % 240) / 240.0f;
        float rgb[4] = { 0.1f + 0.3f * t, 0.2f, 0.5f - 0.3f * t, 1.0f };
        ctx->lpVtbl->ClearRenderTargetView(ctx, rtv, rgb);
        sc->lpVtbl->Present(sc, 1, 0);
        if (++frames % 300 == 0) { printf("frames=%d\n", frames); fflush(stdout); }
        if (frames > 60 * 60 * 5) break;
    }
done:
    printf("exiting after %d frames\n", frames);
    return 0;
}
