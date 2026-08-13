# Decoy steamclient64.dll — Windows x86_64, built with mingw on macOS (#13).
CC     := x86_64-w64-mingw32-gcc
CFLAGS := -shared -O1

steamclient64.dll: probe_steamclient.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f steamclient64.dll shimprobe.log
.PHONY: clean
