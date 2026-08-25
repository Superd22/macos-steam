/* When does winemac.so load? Load user32 (no window), hold, so vmmap can look. */
#include <windows.h>
#include <stdio.h>
int main(int argc, char **argv)
{
    printf("pid=%lu stage=start\n", (unsigned long)GetCurrentProcessId());
    fflush(stdout);
    Sleep(6000);                       /* window A: before user32 */
    HMODULE u = LoadLibraryA("user32.dll");
    printf("user32=%p stage=loaded\n", (void *)u);
    fflush(stdout);
    Sleep(8000);                       /* window B: user32 loaded, no window made */
    if (argc > 1) {                    /* window C: only if asked, touch USER */
        HWND d = GetDesktopWindow();
        printf("desktop=%p stage=used\n", (void *)d);
        fflush(stdout);
    }
    Sleep(10000);
    return 0;
}
