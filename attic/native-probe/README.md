# native-probe (archive) — the #2 go/no-go probes

`probe.cpp`, `machprobe.cpp` and `interpose.cpp`: the throwaway binaries that established a
plain macOS process Steam never launched can `dlopen` `steamclient.dylib`, reach the running
client over live IPC, and read a real achievement with its real unlock time.

**The verdict they produced lives in `instruments/native-probe/FINDINGS.md`**, next to
`connprobe` — that claim is still re-verified after a client update, so the record stays on
the instruments side. Only the superseded binaries are here.

`steam_min.h` is a frozen copy of the one beside `connprobe`. Vtable layouts are transcribed
per client version and never assumed to carry over, so an archived probe keeps the header it
was proven against.

```sh
clang++ -std=c++17 -g -O0 -arch arm64  -o probe-arm64  probe.cpp
clang++ -std=c++17 -g -O0 -arch x86_64 -o probe-x86_64 probe.cpp
clang++ -std=c++17 -g -O0 -arch arm64  -o machprobe-arm64 machprobe.cpp
clang++ -std=c++17 -dynamiclib   -arch arm64 -o interpose.dylib interpose.cpp
```
