# webeasy

Simple cross-platform HTTP server with a console menu on POSIX and a GUI on Windows.

## Build

### Windows (MSVC)

Open a "Developer Command Prompt for VS" and run:

```bat
cl /W4 /O2 /Fe:server.exe main.c /link Ws2_32.lib Shell32.lib
```

### Windows (MinGW-w64)

```bat
gcc -O2 -Wall -Wextra -o server.exe main.c -lws2_32 -lshell32 -lole32
```

### Linux/macOS

```sh
gcc -O2 -Wall -Wextra -pthread -o server main.c
```

## Usage

- On Windows, run `server.exe` to open the GUI.
- On Linux/macOS, run `./server` to open the console menu.
