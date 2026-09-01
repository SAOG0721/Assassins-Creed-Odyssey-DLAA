# Third-party notices

## MinHook 1.3.4

The proxy incorporates MinHook 1.3.4 and its HDE components. MinHook is distributed under its BSD
2-Clause license. The complete copyright notices, conditions, and disclaimers are included in
`LICENSES/MinHook-LICENSE.txt` and `third_party/minhook/LICENSE.txt`.

## NVIDIA RTX SDK / NGX

The bridge uses NVIDIA NGX headers and the NGX import library supplied separately by a source
builder. This source repository does not contain `nvngx_dlss.dll` or `nvngx_dlssnr.dll`.

The v1.0.3 binary package includes unmodified NVIDIA-signed NGX runtime binaries under the NVIDIA
RTX SDK License. Their SHA-256 values are:

```text
BE6E434A94CA32499515EB62CA0E6C274526055D568D0426E4C652DCDFB6EE6E  nvngx_dlss.dll
E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E  nvngx_dlssnr.dll
```

The source repository does not contain these runtime binaries. Source builders must provide their
own version-matched copies and are responsible for provenance, applicable license, integrity and
security.

The NVIDIA license text retained under `LICENSES/` applies according to its own terms; its presence
does not grant rights to unrelated or modified binaries. This project is not sponsored or endorsed
by NVIDIA. NVIDIA software and trademarks remain the property of NVIDIA Corporation.
