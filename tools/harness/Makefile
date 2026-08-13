# Achievement test harness — Windows x86_64, built with mingw on macOS.
#
# steam_api64.dll is Valve's redistributable, NOT committed; `make` copies it
# from the Surviving Mars install in bottle Steam-2 (any SDK-era copy works —
# this one requests STEAMUSERSTATS_INTERFACE_VERSION012 / SteamClient020).
# steam_appid.txt (480) makes the harness init as Spacewar, no install needed.

CC      := x86_64-w64-mingw32-gcc
CFLAGS  := -Wall -Wextra -O1 -static
BOTTLE  := $(HOME)/Library/Application Support/CrossOver/Bottles/Steam-2
MARSDIR := $(BOTTLE)/drive_c/Program Files (x86)/Steam/steamapps/common/Project Spark

all: build/harness.exe build/steam_api64.dll build/steam_appid.txt

build/harness.exe: harness.c | build
	$(CC) $(CFLAGS) -o $@ $<

build/steam_api64.dll: | build
	cp "$(MARSDIR)/steam_api64.dll" $@

build/steam_appid.txt: | build
	printf '480' > $@

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all clean
