# Third-party notices

## MinHook 1.3.4

The proxy incorporates MinHook 1.3.4 and its HDE components. MinHook is distributed under its BSD
2-Clause license. The complete copyright notices, conditions, and disclaimers are included in
`LICENSES/MinHook-LICENSE.txt` and `third_party/minhook/LICENSE.txt`.

## NVIDIA RTX SDK / NGX

The bridge uses NVIDIA NGX headers and the NGX import library supplied separately by a source
builder. This source repository does not contain `nvngx_dlss.dll` or `nvngx_dlssnr.dll`.

The previous v1.0.1 DLAA-only binary release included a production `nvngx_dlss.dll` under the
NVIDIA RTX SDK License. Its SHA-256 was:

```text
BE6E434A94CA32499515EB62CA0E6C274526055D568D0426E4C652DCDFB6EE6E
```

The experimental DLSS 5 source path requires a separately obtained, version-matched
`nvngx_dlssnr.dll`. That runtime is not redistributed by this project. Users and builders are
responsible for its provenance, applicable license, integrity, and security.

The NVIDIA license text retained under `LICENSES/` applies according to its own terms; its presence
does not grant rights to unrelated or modified binaries. This project is not sponsored or endorsed
by NVIDIA. NVIDIA software and trademarks remain the property of NVIDIA Corporation.
