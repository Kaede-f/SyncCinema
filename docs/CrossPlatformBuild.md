# Cross-Platform Build Notes

SyncCinema currently uses one shared codebase for two deployment roles:

- Windows client: opens local video with libVLC and connects to a server.
- Linux server: accepts clients and broadcasts playback commands.

## Encoding Rules

All source files should be saved as UTF-8 with LF line endings. This avoids
Windows/WSL editing problems where comments and code can accidentally merge
onto one line.

The repository uses:

- `.editorconfig` for editor behavior.
- `.gitattributes` for Git line-ending normalization.
- MSVC `/utf-8` compile option for Windows builds.

## Windows Client Build

Use Visual Studio CMake integration:

1. Select `x64 Debug with libVLC`.
2. Run `Build -> Build All`.
3. Start the client:

```bat
out\build\x64-vlc-debug\SyncCinema.exe --client "D:\videos\test.mp4"
```

## Linux Server Build

On Ubuntu 22.04:

```bash
sudo apt update
sudo apt install -y git cmake g++ build-essential
cmake --preset linux-debug
cmake --build out/build/linux-debug --target SyncCinemaServer
./out/build/linux-debug/SyncCinemaServer
```

If presets are not available, use the plain CMake form:

```bash
cmake -S . -B build
cmake --build build --target SyncCinemaServer
./build/SyncCinemaServer
```

## Recommended Verification

Before pushing cross-platform changes:

1. Build the Windows libVLC client in Visual Studio.
2. Build `SyncCinemaServer` on WSL or the cloud server.
3. Run two Windows clients and confirm `play`, `pause`, and `seek <seconds>`
   are broadcast through the Linux server.
