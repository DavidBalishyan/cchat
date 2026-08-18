# C-Chat

> [!NOTE]
> This is a fork of <https://github.com/DavidBalishyan/C-Chat>.

C-Chat is a terminal chat program with a C server and an Erlang client. The
server requires a password and adds the sender's username to each message before
passing it to the other authenticated clients. It accepts up to 10 connections
at a time by default.

[Watch the demo](https://mega.nz/file/G3gRyCbb#mziMURUhjOgjdmDOo4gTBonqxGnJWyfNRoD_-6CAF-0).

## Requirements

- A POSIX system such as Linux, macOS, or BSD
- A C11 compiler (`cc`, GCC, or Clang)
- Erlang/OTP 27 or newer
- Python 3 for the included build tool

## Build

Makeover is included in `scripts/makeover`, so you do not need to install a
separate build tool. From the project directory, run:

```sh
./scripts/makeover
```

Makeover writes the programs to `build/server` and `build/client`. To see the
other build targets, run:

```sh
./scripts/makeover --list
```

The available targets include `server`, `client`, `check`, and `clean`. To build
everything and run the checks:

```sh
./scripts/makeover check
```

## Run

The server reads its password from `C_CHAT_PASSWORD`. This starts it on the
default port, 8080:

```sh
C_CHAT_PASSWORD='choose-a-password' ./build/server
```

Pass a port number to use a different one. Port `0` lets the operating system
choose an available port.

```sh
C_CHAT_PASSWORD='choose-a-password' ./build/server 9000
```

Start a client with the server address and a username. The password prompt does
not echo what you type.

```sh
./build/client 127.0.0.1:8080 alice
```

Enter `/quit` to disconnect the client. Ctrl-C or SIGTERM stops the server
cleanly.

> [!WARNING]
> The server password travels over the TCP connection without encryption. Keep
> C-Chat on a trusted network, or run it through a VPN or SSH tunnel. Do not
> expose the server directly to the internet without TLS.
