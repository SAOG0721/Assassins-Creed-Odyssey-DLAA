# Building from source

The end-user guide is in `README.md`. This file only covers reproducible local builds.

## Requirements

- Windows 10/11 x64
- Visual Studio 2022 with **Desktop development with C++**
- CMake 3.21 or newer
- A Windows SDK containing `fxc.exe`
- NVIDIA RTX SDK / NGX SDK headers and `nvsdk_ngx_d.lib`
- A production `nvngx_dlss.dll` covered by the NVIDIA RTX SDK License

MinHook 1.3.4 source is vendored under `third_party/minhook` with its BSD 2-Clause license.

## Build

```powershell
.\build.ps1 `
  -NgxSdkRoot 'C:\path\to\ngx-sdk' `
  -NgxRuntime 'C:\path\to\nvngx_dlss.dll'
```

You can optionally pass `-CMakePath`, `-FxcPath`, or `-MinHookRoot`. Outputs are written to
`build/bin`.

## Tests

After a Release build:

```powershell
.\build\Release\d3d11-vtable-indices.exe
.\build\Release\execute-detour-smoke.exe
.\build\Release\bridge-smoke.exe .\build\bin\ACOdysseyDLSSBridge.dll
.\build\Release\ngx-synthetic-smoke.exe .\build\bin\ACOdysseyDLSSBridge.dll
```

All four commands must exit with code `0`. The synthetic NGX test requires compatible NVIDIA
hardware and a working display driver.

The proxy is deliberately gated to the audited Steam executable SHA-256 documented in `README.md`.
