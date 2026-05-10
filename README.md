# CPUMax

Tiny Windows tray utility to toggle the **Maximum Processor State** power setting.

## Features
- System tray icon (green = enabled, red = disabled)
- Right-click menu: Enable / Disable / Start with Windows / Install / Exit
- Double-click tray icon to toggle
- Hover tooltip shows status + live CPU and RAM usage
- Single .exe, no dependencies, ~50 KB
- Optional one-click "Install" copies itself to `%LOCALAPPDATA%\CPUMax` and adds a startup entry

## Build

You need a C compiler. Easiest option:

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT
```

Then from this folder:

```powershell
.\build.bat
```

Output: `build\CPUMax.exe`

## Run

Just double-click `build\CPUMax.exe`. Look for the dot in your tray.

To install permanently: right-click the tray icon -> **Install to %LOCALAPPDATA%**, then **Start with Windows**.

## How "Enabled / Disabled" works

- **Enabled**  - sets `Maximum processor state` = 100% (turbo allowed)
- **Disabled** - sets it to 99% (a common trick to suppress Intel Turbo Boost)

Both AC and DC profiles are written, and the active power scheme is re-applied so the change takes effect immediately.

## Notes
- HKCU startup entry is per-user, no admin required.
- Power changes use the documented `PowerWrite*ValueIndex` API in `powrprof.dll`.
- No telemetry, no network, no installer — just a single .exe.
