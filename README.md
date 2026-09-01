# Assassin's Creed Odyssey DLAA + DLSS 5

Unofficial native-data NVIDIA DLAA and DLSS 5 Neural Rendering integration for the DirectX 11
Steam version of *Assassin's Creed Odyssey*.

[English](#english) | [中文](#中文)

## English

### What this project does

- Adds full-resolution NVIDIA DLAA while keeping the original game TAA as a fail-safe baseline.
- Adds a DLSS 5 Neural Rendering path through a private same-adapter D3D12 bridge.
- Press `F8` to select Game TAA or the DLAA/DLSS 5 route.
- Press `F9` to open the persistent in-game DLSS 5 control panel.
- Applies the existing fixed `0.2` luma-preserving post-process sharpening to the selected neural
  output.
- Never skips the original TAA draw and does not modify the game executable or archives.

### Native TAA data, not estimated guides

This integration captures the additional data already produced for Odyssey's original TAA pass:

- full-resolution game color;
- the game's native packed per-pixel motion vectors;
- the game's native full-resolution depth;
- the game's real temporal jitter and projection constants.

Motion is deterministically decoded from the game's packed representation and converted to the
units/direction required by NGX. Depth is deterministically converted using the game's captured
projection data. **There is no estimated depth and no estimated or generated motion-vector path.**

DLAA and DLSS 5 therefore receive the game's own temporally aligned Color, Depth, Motion and jitter,
the same data class an engine-native integration uses. On the validated game build, the DLAA and
DLSS 5 result matches native-engine integration behavior; it is not an approximation driven by
reconstructed guides.

### Pipeline

```text
Original Odyssey TAA command list executes normally
  -> capture native Color / packed Motion / Depth / jitter
  -> full-resolution D3D11 DLAA
  -> optional same-adapter D3D12 DLSS 5 Feature 18 evaluation
  -> return the selected full-resolution result to Odyssey post-processing
  -> UI / Present
```

The D3D12 path uses D3D12-owned simultaneous-access FP16 resources and a shared GPU fence. Its
static D3D12 core owns parameter allocation/destruction, while the version-matched direct runtime
owns Feature 18 Init/Create/Evaluate/Release. Any NR-only error immediately publishes DLAA instead.

### Verified environment and evidence

- Windows 11, DirectX 11 game path.
- NVIDIA GeForce RTX 5070 Ti.
- Tested on Steam build `17083392` of *Assassin's Creed Odyssey*.
- In-game validation at 2560×1440 completed more than 25,000 successful DLSS 5 evaluations after
  warmup with `publish=1` and NGX success result `0x00000001`.
- Offline real-GPU tests reached D3D12 core init, direct runtime init, parameter allocation,
  Feature 18 evaluation, fence completion, Feature release, parameter destruction and both shutdowns.
- D3D11/D3D12 shared FP16 resources and the shared-fence round trip were tested independently.

The mod does not enforce an executable hash, which may improve compatibility across Steam builds.
It still requires the expected temporal shader/resource contract and keeps Game TAA when a compatible
path is unavailable. A game update that changes the relevant shaders can therefore break compatibility.
The mod is currently intended only for the Steam version; other editions are not supported.

### Source and release status

The `main` branch contains the DLAA + DLSS 5 source. The current binary package is
[v1.0.3](https://github.com/SAOG0721/Assassins-Creed-Odyssey-DLAA/releases/tag/v1.0.3).

The version-matched NGX runtime binaries are not stored in the source repository. The v1.0.3 binary
package includes the runtime versions listed in its third-party notice. Source builders must provide
their own copies and are responsible for provenance, license and security; an arbitrary DLL with the
same filename is not supported.

### Controls

- `F8`: switch between Game TAA and the DLAA/DLSS 5 route.
- `F9`: open or close the persistent DLSS 5 control panel.
- With DLSS 5 disabled, the neural route presents DLAA.
- After enabling DLSS 5, eight successful private warmup frames are required before NR is published.
- Settings are saved to `ACOdysseyDLAA.ini`.

The normal F9 view exposes Preset, Style, Intensity, Local Tone, Local Structure, Skin Structure,
Automatic Mask and UI Correction. Motion, depth and color-bridge controls are under **Advanced**.

### Configuration

The shipped configuration starts with Game TAA visible and DLSS 5 disabled.

| Section/key | Default | Description |
| --- | ---: | --- |
| `DLAA/Enable` | `1` | Enable the DLAA bridge; failure keeps Game TAA. |
| `DLAA/EvaluateOnly` | `1` | Start with Game TAA visible. |
| `DLAA/PresentationToggleKey` | `119` | Decimal Win32 key code for `F8`. |
| `DLSSNR/Enable` | `0` | Enable DLSS 5; normally controlled from F9. |
| `DLSSNR/Preset` | `0` | Version-bound neural preset. |
| `DLSSNR/Style` | `0` | Version-bound style. |
| `DLSSNR/Intensity` | `1.0` | Neural effect intensity. |
| `DLSSNR/LocalToneStrength` | `1.0` | Local tone control. |
| `DLSSNR/LocalStructureStrength` | `1.0` | Local structure control. |
| `DLSSNR/SkinStructureStrength` | `-1.0` | Skin structure control. |
| `DLSSNR/UseAutoMask` | `1` | Automatic mask. |
| `DLSSNR/UICorrection` | `1` | UI correction. |
| `DLSSNR.Advanced/DepthConvention` | `0` | Use the audited game depth convention. |
| `DLSSNR.Advanced/MotionScaleX/Y` | `1.0` | Multipliers over the audited native MV scale. |
| `DLSSNR.Advanced/ControlCompatibleColorTransfer` | `0` | Optional color-transfer bridge. |
| `DLSSNR.Advanced/ScenePaperWhiteScale` | `1.0` | Scene paper-white scale. |
| `DLSSNR.Advanced/HDRTransferStrength` | `1.0` | Color-transfer strength. |
| `DLSSNR.Advanced/ColorStrength` | `1.0` | Final color-bridge strength. |

### Building

See [BUILDING.md](BUILDING.md). A source build requires Visual Studio 2022, CMake, the Windows SDK
with `fxc.exe`, the NVIDIA NGX SDK, `nvngx_dlss.dll`, and a separately obtained version-matched
`nvngx_dlssnr.dll`. MinHook is included in the source tree under its BSD 2-Clause license.

### Tests

The repository contains five standalone checks:

```powershell
.\build\Release\d3d11-vtable-indices.exe
.\build\Release\execute-detour-smoke.exe
.\build\Release\d3d11-d3d12-interop-smoke.exe
$bridge = (Resolve-Path .\build\bin\ACOdysseyDLSSBridge.dll).Path
.\build\Release\bridge-smoke.exe $bridge
.\build\Release\ngx-synthetic-smoke.exe $bridge
```

The synthetic test uses real hardware and a real NR-enabled frame; it is not a mock. It validates
bootstrap and submission, but synthetic input does not replace in-game motion, depth, output and
visual validation.

### Installation and safety

For normal installation, download the
[v1.0.3 binary package](https://github.com/SAOG0721/Assassins-Creed-Odyssey-DLAA/releases/tag/v1.0.3)
and extract it beside `ACOdyssey.exe`. Source builders must place the same ten generated/runtime files
there: the two DLLs, five `.cso` shaders, both NGX runtimes and `ACOdysseyDLAA.ini`. Back up same-name
files first and never overwrite another mod's `dinput8.dll`.

```text
dinput8.dll
ACOdysseyDLSSBridge.dll
ACOdysseyDLAA.motion_decode.cso
ACOdysseyDLAA.sharpen.cso
ACOdysseyDLSSNR.encode.cso
ACOdysseyDLSSNR.decode.cso
ACOdysseyDLSSNR.guide_unpack.cso
nvngx_dlss.dll
nvngx_dlssnr.dll
ACOdysseyDLAA.ini
```

Copy the repository `config.ini` as `ACOdysseyDLAA.ini`. To uninstall, close the game and remove
only these files when they are known to belong to this project.

The current source does not gate on the game executable hash. It fails closed when the expected
runtime shader/resource contract is unavailable or evaluation fails, retaining Game TAA. It is
intended for the offline single-player game and does not bypass anti-cheat or online integrity
systems.

### Logs and troubleshooting

The mod writes `ACOdysseyDLAA.log` and `ACOdysseyDLSSBridge.log` beside the game executable.

- If Game TAA remains visible, check for `ERROR`, `DLSSNR_FALLBACK` or failed NGX result lines.
- If F9 reports warmup, allow the required successful frames before judging NR output.
- If startup fails, remove only files belonging to this mod; original game files are not modified.

### Current package integrity

`ACOdysseyDLAA-v1.0.3.zip` SHA-256:

```text
a8bc422756d6b05c35c9c0f627a766b98ba56b5fe09b674a5fb962f022d7d699
```

### Third-party software and disclaimer

The project uses NVIDIA NGX interfaces and incorporates MinHook. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and `LICENSES/`.

This is an independent, unofficial project. It is not affiliated with or endorsed by Ubisoft,
NVIDIA, Valve, or their subsidiaries. All product names and trademarks belong to their respective
owners. Use the project at your own risk.

---

## 中文

### 项目功能

- 为《刺客信条：奥德赛》加入全分辨率 NVIDIA DLAA，并保留原版 TAA 作为故障安全基线。
- 通过同显卡私有 D3D12 bridge 加入 DLSS 5 Neural Rendering。
- 按 `F8` 选择原版 TAA 或 DLAA/DLSS 5 路径。
- 按 `F9` 打开持久化的游戏内 DLSS 5 控制面板。
- 对选中的神经网络输出应用既有的固定 `0.2` 保亮度后级锐化。
- 原版 TAA draw 始终正常执行，不修改游戏 EXE 或原始封包。

### 捕获原生 TAA 数据，不估算 Guide

本项目直接捕获《奥德赛》原生 TAA pass 已经生成的附加数据：

- 全分辨率游戏 Color；
- 游戏原生逐像素打包 Motion Vectors；
- 游戏原生全分辨率 Depth；
- 游戏实际使用的时序 jitter 和投影常量。

Motion 只从游戏原生打包格式确定性解码，再转换为 NGX 所需的单位和方向；Depth 只使用捕获的
游戏投影数据进行确定性格式转换。**没有任何估算深度，也没有任何估算或生成运动矢量的路径。**

因此 DLAA 与 DLSS 5 获得的是游戏自身、按同一时序帧对齐的 Color、Depth、Motion 和 jitter，
属于原生引擎接入所使用的同类数据。在已验证游戏版本上，DLAA 与 DLSS 5 的效果与原生引擎接入
一致；这不是依赖重建 Guide 得到的近似实现。

### 管线结构

```text
《奥德赛》原版 TAA command list 正常执行
  -> 捕获原生 Color / 打包 Motion / Depth / jitter
  -> 全分辨率 D3D11 DLAA
  -> 可选同显卡 D3D12 DLSS 5 Feature 18 Evaluate
  -> 将选中的全分辨率结果交还《奥德赛》后处理
  -> UI / Present
```

D3D12 路径使用 D3D12 所有的 simultaneous-access FP16 资源和共享 GPU fence。静态 D3D12 core
负责参数 Allocate/Destroy，版本匹配的 direct runtime 负责 Feature 18 的
Init/Create/Evaluate/Release。任何仅 NR 路径的错误都会立即改用 DLAA 输出。

### 已验证环境与证据

- Windows 11，游戏 DirectX 11 路径。
- NVIDIA GeForce RTX 5070 Ti。
- 已测试 Steam 版《刺客信条：奥德赛》build `17083392`。
- 2560×1440 游戏内验证在 warmup 后完成超过 25,000 次成功 DLSS 5 Evaluate，持续
  `publish=1`，NGX 成功结果为 `0x00000001`。
- 真实 GPU 离线测试已到达 D3D12 core 初始化、direct runtime 初始化、参数分配、Feature 18
  Evaluate、fence 完成、Feature 释放、参数销毁和两套 Shutdown。
- D3D11/D3D12 共享 FP16 资源和共享 fence 往返已单独验证。

模组不强制校验游戏 EXE 哈希，因此可能提高不同 Steam 版本之间的兼容性。模组仍要求预期的运行时
时域 shader/资源契约；未找到兼容路径时保留原版 TAA。如果游戏更新改变对应 shader，当前版本仍
可能失效。目前仅适用于 Steam 版本，不支持其他商店版本。

### 源码与 Release 状态

`main` 分支包含 DLAA + DLSS 5 源码。当前二进制包为
[v1.0.3](https://github.com/SAOG0721/Assassins-Creed-Odyssey-DLAA/releases/tag/v1.0.3)。

版本匹配的 NGX 运行库不存放在源码仓库中。v1.0.3 二进制包包含其第三方声明中列出的运行库版本。
源码构建者必须自行准备运行库，并自行负责其来源、许可和安全；不能把任意同名 DLL 视为受支持版本。

### 控制方式

- `F8`：在原版 TAA 与 DLAA/DLSS 5 路径之间切换。
- `F9`：打开或关闭持久化 DLSS 5 控制面板。
- DLSS 5 关闭时，神经网络路径显示 DLAA。
- 开启 DLSS 5 后，先完成 8 个私有成功 warmup 帧，再发布 NR 输出。
- 设置保存在 `ACOdysseyDLAA.ini`。

F9 普通界面提供 Preset、Style、Intensity、Local Tone、Local Structure、Skin Structure、
Automatic Mask 和 UI Correction。Motion、Depth 和色彩桥参数折叠在“高级”中。

### 配置

默认配置以原版 TAA 启动，并关闭 DLSS 5。

| 配置项 | 默认值 | 说明 |
| --- | ---: | --- |
| `DLAA/Enable` | `1` | 启用 DLAA bridge；失败时保留原版 TAA。 |
| `DLAA/EvaluateOnly` | `1` | 启动时显示原版 TAA。 |
| `DLAA/PresentationToggleKey` | `119` | `F8` 的十进制 Win32 键码。 |
| `DLSSNR/Enable` | `0` | 启用 DLSS 5；通常由 F9 控制。 |
| `DLSSNR/Preset` | `0` | 与运行库版本绑定的神经网络预设。 |
| `DLSSNR/Style` | `0` | 与运行库版本绑定的风格。 |
| `DLSSNR/Intensity` | `1.0` | 神经效果强度。 |
| `DLSSNR/LocalToneStrength` | `1.0` | 局部色调控制。 |
| `DLSSNR/LocalStructureStrength` | `1.0` | 局部结构控制。 |
| `DLSSNR/SkinStructureStrength` | `-1.0` | 皮肤结构控制。 |
| `DLSSNR/UseAutoMask` | `1` | 自动遮罩。 |
| `DLSSNR/UICorrection` | `1` | UI 修正。 |
| `DLSSNR.Advanced/DepthConvention` | `0` | 使用已审计的游戏深度约定。 |
| `DLSSNR.Advanced/MotionScaleX/Y` | `1.0` | 对已审计原生 MV scale 的倍率。 |
| `DLSSNR.Advanced/ControlCompatibleColorTransfer` | `0` | 可选色彩传递桥。 |
| `DLSSNR.Advanced/ScenePaperWhiteScale` | `1.0` | 场景 paper-white 比例。 |
| `DLSSNR.Advanced/HDRTransferStrength` | `1.0` | 色彩传递强度。 |
| `DLSSNR.Advanced/ColorStrength` | `1.0` | 最终色彩桥强度。 |

### 构建

参见 [BUILDING.md](BUILDING.md)。源码构建需要 Visual Studio 2022、CMake、带 `fxc.exe` 的
Windows SDK、NVIDIA NGX SDK、`nvngx_dlss.dll`，以及另行取得的版本匹配
`nvngx_dlssnr.dll`。MinHook 源码按 BSD 2-Clause 许可证随仓库提供。

### 测试

仓库包含五个独立检查程序：

```powershell
.\build\Release\d3d11-vtable-indices.exe
.\build\Release\execute-detour-smoke.exe
.\build\Release\d3d11-d3d12-interop-smoke.exe
$bridge = (Resolve-Path .\build\bin\ACOdysseyDLSSBridge.dll).Path
.\build\Release\bridge-smoke.exe $bridge
.\build\Release\ngx-synthetic-smoke.exe $bridge
```

合成测试使用真实硬件并真正开启一个 NR 帧，不是 mock。它验证启动与提交，但不能替代游戏内真实
Motion、Depth、输出接管和画质验证。

### 安装与安全

普通安装请下载
[v1.0.3 二进制包](https://github.com/SAOG0721/Assassins-Creed-Odyssey-DLAA/releases/tag/v1.0.3)
并解压到 `ACOdyssey.exe` 同级目录。源码构建者也需要把相同的十个构建/运行文件放到该目录：
两个 DLL、五个 `.cso` shader、两个 NGX runtime 和 `ACOdysseyDLAA.ini`。安装前备份同名文件，
不要覆盖其他模组的 `dinput8.dll`。

```text
dinput8.dll
ACOdysseyDLSSBridge.dll
ACOdysseyDLAA.motion_decode.cso
ACOdysseyDLAA.sharpen.cso
ACOdysseyDLSSNR.encode.cso
ACOdysseyDLSSNR.decode.cso
ACOdysseyDLSSNR.guide_unpack.cso
nvngx_dlss.dll
nvngx_dlssnr.dll
ACOdysseyDLAA.ini
```

把仓库中的 `config.ini` 复制为 `ACOdysseyDLAA.ini`。卸载时先关闭游戏，并且只删除确认属于
本项目的这些文件。

当前源码不以游戏 EXE 哈希作为门禁；运行时 shader/资源契约不匹配或 Evaluate 失败时会
fail closed 并保留原版 TAA。本项目只面向离线单人游戏，不绕过反作弊或在线完整性系统。

### 日志与故障排查

模组会在游戏 EXE 同级目录生成 `ACOdysseyDLAA.log` 和 `ACOdysseyDLSSBridge.log`。

- 如果仍显示原版 TAA，检查 `ERROR`、`DLSSNR_FALLBACK` 或失败的 NGX 结果。
- 如果 F9 显示 warmup，等待规定数量的成功帧后再判断 NR 输出。
- 如果启动异常，只删除属于本模组的文件；本项目不会修改原始游戏文件。

### 当前安装包完整性

`ACOdysseyDLAA-v1.0.3.zip` SHA-256：

```text
a8bc422756d6b05c35c9c0f627a766b98ba56b5fe09b674a5fb962f022d7d699
```

### 第三方软件与免责声明

项目使用 NVIDIA NGX 接口并包含 MinHook。详情见
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 和 `LICENSES/`。

本项目是独立制作的非官方项目，与 Ubisoft、NVIDIA、Valve 及其关联公司不存在隶属、合作或背书
关系。所有产品名称和商标均归各自权利人所有。使用风险由用户自行承担。
