# Building from source

The end-user and architecture overview is in `README.md`. This file covers reproducible local
source builds. The repository does not contain NVIDIA runtime DLLs.

## Requirements

- Windows 10/11 x64
- Visual Studio 2022 with **Desktop development with C++**
- CMake 3.21 or newer
- A Windows SDK containing `fxc.exe`
- NVIDIA RTX SDK / NGX SDK headers and `nvsdk_ngx_d.lib`
- A production `nvngx_dlss.dll` obtained under applicable NVIDIA terms
- A separately obtained, version-matched `nvngx_dlssnr.dll` for the experimental DLSS 5 path

MinHook 1.3.4 source is vendored under `third_party/minhook` with its BSD 2-Clause license.

## Build

```powershell
.\build.ps1 `
  -NgxSdkRoot 'C:\path\to\ngx-sdk' `
  -NgxRuntime 'C:\path\to\nvngx_dlss.dll' `
  -DlssNrRuntime 'C:\path\to\nvngx_dlssnr.dll'
```

The same values can be supplied through `NGX_SDK_ROOT`, `NGX_RUNTIME`, and `DLSSNR_RUNTIME`
environment variables. You can optionally pass `-CMakePath`, `-FxcPath`, or `-MinHookRoot`.
Outputs are written to `build/bin`.

The DLSS 5 compatibility code calls version-bound private exports and is not a stable public NGX
contract. Do not substitute an arbitrary DLL with the same filename. This repository does not grant
rights to obtain, modify, or redistribute NVIDIA binaries.

## Tests

After a Release build:

```powershell
.\build\Release\d3d11-vtable-indices.exe
.\build\Release\execute-detour-smoke.exe
.\build\Release\d3d11-d3d12-interop-smoke.exe
$bridge = (Resolve-Path .\build\bin\ACOdysseyDLSSBridge.dll).Path
.\build\Release\bridge-smoke.exe $bridge
.\build\Release\ngx-synthetic-smoke.exe $bridge
```

All five commands must exit with code `0`. The interop and synthetic NGX tests require compatible
hardware and a working display driver. The synthetic test enables NR for one real-GPU frame and
asserts the successful warmup state, a positive Evaluate count, NGX success, and balanced explicit
shutdown. It does not prove in-game guide semantics or visual quality.

## Runtime safety boundary

The current proxy does not enforce a game-executable hash. It validates the live temporal shader,
resource and constant-buffer contract at runtime. The original Game TAA is preserved when a
compatible path is not found or any DLAA/DLSS 5 stage fails.
