CC = cc
ESCRIPT = escript
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow

.PHONY: all server client check clean

[group: Build]
# Build the server and client binaries.
all: build/server build/client

# Build only the C chat server.
server: build/server

# Build only the Erlang chat client.
client: build/client

build/server: server/main.c server/loader.h
	@mkdir -p build
	$(CC) $(CFLAGS) server/main.c -o $@

build/client: client/main.escript
	@mkdir -p build
	@$(ESCRIPT) client/main.escript >/dev/null
	cp client/main.escript $@
	chmod +x $@

[group: Quality]
# Build everything and run static checks.
check: all

[group: Maintenance]
# Remove generated build artifacts.
clean:
	rm -rf build
