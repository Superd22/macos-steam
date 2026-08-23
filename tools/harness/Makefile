# Achievement test harness — Windows x86_64 AND i386, built with mingw on macOS.
#
# steam_api64.dll is Valve's redistributable, NOT committed; `make` copies it
# from the Surviving Mars install in bottle Steam-2 (any SDK-era copy works —
# this one requests STEAMUSERSTATS_INTERFACE_VERSION012 / SteamClient020).
# steam_appid.txt (480) makes the harness init as Spacewar, no install needed.

# The 32-bit harness (#20) is the same source: it resolves every export through
# GetProcAddress and its CallbackMsg_t is declared with a native pointer, so it
# is correct for whichever bitness it is compiled as. Its Valve redistributable
# is the 32-bit steam_api.dll, taken from an installed 32-bit title (Among Us).
# Together they exercise the i386 side of the shim on Spacewar — an achievement
# round-trip that needs no game installed and burns nothing.
CC      := x86_64-w64-mingw32-gcc
CC32    := i686-w64-mingw32-gcc
CFLAGS  := -Wall -Wextra -O1 -static
BOTTLE  := $(HOME)/Library/Application Support/CrossOver/Bottles/Steam-2
MARSDIR := $(BOTTLE)/drive_c/Program Files (x86)/Steam/steamapps/common/Project Spark
AUDIR   := $(HOME)/Library/Application Support/Steam/steamapps/common/Among Us/Among Us_Data/Plugins/x86

all: build/harness.exe build/steam_api64.dll build/steam_appid.txt
all32: build32/harness.exe build32/steam_api.dll build32/steam_appid.txt

build/harness.exe: harness.c | build
	$(CC) $(CFLAGS) -o $@ $<

build/steam_api64.dll: | build
	cp "$(MARSDIR)/steam_api64.dll" $@

build/steam_appid.txt: | build
	printf '480' > $@

build32/harness.exe: harness.c | build32
	$(CC32) $(CFLAGS) -o $@ $<

build32/steam_api.dll: | build32
	cp "$(AUDIR)/steam_api.dll" $@

build32/steam_appid.txt: | build32
	printf '480' > $@

build build32:
	mkdir -p $@

clean:
	rm -rf build build32

.PHONY: all all32 clean
