# Assassin's Creed Odyssey DLAA

Unofficial NVIDIA DLAA mod for the DirectX 11 Steam version of *Assassin's Creed Odyssey*.

[English](#english) | [中文](#中文)

## English

### Features

- Adds full-resolution NVIDIA DLAA while preserving the game's original TAA as a fallback.
- Starts with Game TAA visible. Press `F8` to switch between Game TAA and DLAA.
- Applies a fixed `0.2` post-DLAA sharpening pass.
- Keeps DLAA history warm while Game TAA is displayed, so switching does not start with cold history.
- Automatically keeps Game TAA when DLAA initialization or evaluation fails.
- Does not modify the game executable or original game archives.

This is a 1:1 DLAA replacement for anti-aliasing, not DLSS Super Resolution. It does not lower the
game's internal rendering resolution and should not be expected to improve frame rate.

### Requirements and compatibility

- Windows 10 or Windows 11, 64-bit.
- An NVIDIA RTX GPU and a current NVIDIA display driver.
- Steam build `17083392` of *Assassin's Creed Odyssey*.
- Verified `ACOdyssey.exe` SHA-256:
  `AC327DAD2CBBDD72A3FDA8E99CBEAB9D12AF328363E4F09BC5674BDD36B8C483`.

The mod intentionally stays inactive when the executable hash does not match. Other editions,
future game updates, or mods that also install `dinput8.dll` are not currently supported.

### Download and installation

1. Download `ACOdysseyDLAA-v1.0.0.zip` from the
   [v1.0.0 release](https://github.com/SAOG0721/Assassins-Creed-Odyssey-DLAA/releases/tag/v1.0.0).
2. Close the game.
3. Open the game directory containing `ACOdyssey.exe` (Steam → Library → right-click the game →
   Manage → Browse local files).
4. Back up or remove any existing file with the same name as one of the files below. Do not
   overwrite another mod's `dinput8.dll`.
5. Extract these six runtime files beside `ACOdyssey.exe`:

```text
dinput8.dll
ACOdysseyDLSSBridge.dll
ACOdysseyDLAA.motion_decode.cso
ACOdysseyDLAA.sharpen.cso
nvngx_dlss.dll
ACOdysseyDLAA.ini
```

### Use

- Launch the game normally.
- The game begins with its original TAA visible.
- Press `F8` while the game window is focused to switch to DLAA; press it again to restore Game TAA.
- `F8` is the only runtime shortcut. There is no calibration window or additional hotkey.

### Configuration

Edit `ACOdysseyDLAA.ini` while the game is closed.

| Section/key | Default | Description |
| --- | ---: | --- |
| `Probe/MaxTaaDrawLogs` | `0` | Number of detailed TAA draw records; `0` disables them. |
| `DLAA/Enable` | `1` | Loads and evaluates the DLAA bridge. Set to `0` to leave only Game TAA active. |
| `DLAA/EvaluateOnly` | `1` | `1` starts with Game TAA visible; `0` starts with DLAA visible. |
| `DLAA/AllowPresentationToggle` | `1` | Enables the foreground-only presentation hotkey. |
| `DLAA/PresentationToggleKey` | `119` | Decimal Win32 virtual-key code. `119` is `F8`. |
| `DLAA/DepthInverted` | `0` | Validated depth convention. Leave this at `0`. |

The jitter, motion-vector convention, DLAA mode, and sharpening strength are fixed to the validated
values and are not user-adjustable.

### Logs and troubleshooting

The mod creates `ACOdysseyDLAA.log` and `ACOdysseyDLSSBridge.log` in the game directory.

- No effect: confirm the game build and executable hash, check for another `dinput8.dll` mod, and
  update the NVIDIA driver.
- Game TAA remains visible after pressing `F8`: inspect the two logs for `ERROR` lines. The mod
  deliberately falls back instead of replacing the output after a failed DLAA evaluation.
- Startup problem: remove the six runtime files listed above. The original game files are untouched.

### Uninstall

Close the game and delete only the six runtime files listed in the installation section. You may
also delete `ACOdysseyDLAA.log` and `ACOdysseyDLSSBridge.log`. Do not delete a file that belongs to
another mod.

### Package integrity

`ACOdysseyDLAA-v1.0.0.zip` SHA-256:

```text
09c1de2e8824b99634e2a985029bd9d89b2a952e310fae1d531a8a5a85cacdee
```

Per-file hashes are included in `SHA256SUMS.txt` inside the archive. The archive checksum is also
available as the `ACOdysseyDLAA-v1.0.0-SHA256SUMS.txt` release attachment.

### Third-party software and disclaimer

The package includes NVIDIA `nvngx_dlss.dll` under the NVIDIA RTX SDK License and incorporates
MinHook under its BSD 2-Clause license. Full terms are included under `LICENSES/` and summarized in
`THIRD_PARTY_NOTICES.md`.

This is an independent, unofficial project. It is not affiliated with or endorsed by Ubisoft,
NVIDIA, Valve, or their subsidiaries. All product names and trademarks belong to their respective
owners. Use the mod at your own risk.

---

## 中文

### 功能

- 为《刺客信条：奥德赛》加入全分辨率 NVIDIA DLAA，并保留原版 TAA 作为故障回退。
- 启动时显示原版 TAA；按 `F8` 在原版 TAA 与 DLAA 之间切换。
- DLAA 输出使用固定强度 `0.2` 的后级锐化。
- 显示原版 TAA 时仍维持 DLAA 历史，切换时不会从冷历史开始。
- DLAA 初始化或 Evaluate 失败时自动保留原版 TAA。
- 不修改游戏 EXE 或原始游戏封包。

这是 1:1 分辨率的 DLAA 抗锯齿替换，不是 DLSS 超分辨率。它不会降低游戏内部渲染分辨率，
因此不应期待它提高帧率。

### 系统要求与兼容性

- 64 位 Windows 10 或 Windows 11。
- NVIDIA RTX 显卡和较新的 NVIDIA 显卡驱动。
- Steam 版《刺客信条：奥德赛》build `17083392`。
- 已验证的 `ACOdyssey.exe` SHA-256：
  `AC327DAD2CBBDD72A3FDA8E99CBEAB9D12AF328363E4F09BC5674BDD36B8C483`。

EXE 哈希不匹配时模组会保持不活动。目前不支持其他商店版本、未来游戏更新，或同样安装
`dinput8.dll` 的其他模组。

### 下载与安装

1. 从 [v1.0.0 Release](https://github.com/SAOG0721/Assassins-Creed-Odyssey-DLAA/releases/tag/v1.0.0)
   下载 `ACOdysseyDLAA-v1.0.0.zip`。
2. 关闭游戏。
3. 打开包含 `ACOdyssey.exe` 的游戏目录（Steam → 库 → 右键游戏 → 管理 → 浏览本地文件）。
4. 如果目录中已有下列同名文件，请先确认归属并自行备份；不要覆盖其他模组的 `dinput8.dll`。
5. 将以下六个运行文件解压到 `ACOdyssey.exe` 同级目录：

```text
dinput8.dll
ACOdysseyDLSSBridge.dll
ACOdysseyDLAA.motion_decode.cso
ACOdysseyDLAA.sharpen.cso
nvngx_dlss.dll
ACOdysseyDLAA.ini
```

### 使用

- 正常启动游戏。
- 游戏默认显示原版 TAA。
- 游戏窗口位于前台时按 `F8` 切换到 DLAA；再次按下恢复原版 TAA。
- `F8` 是唯一运行时快捷键，没有校准窗口或其他快捷键。

### 配置

关闭游戏后编辑 `ACOdysseyDLAA.ini`。

| 配置项 | 默认值 | 说明 |
| --- | ---: | --- |
| `Probe/MaxTaaDrawLogs` | `0` | TAA draw 详细日志数量；`0` 表示关闭。 |
| `DLAA/Enable` | `1` | 加载并运行 DLAA bridge；设为 `0` 时只保留原版 TAA。 |
| `DLAA/EvaluateOnly` | `1` | `1` 表示启动时显示原版 TAA；`0` 表示启动时显示 DLAA。 |
| `DLAA/AllowPresentationToggle` | `1` | 启用仅在游戏位于前台时生效的切换键。 |
| `DLAA/PresentationToggleKey` | `119` | 十进制 Win32 虚拟键码；`119` 代表 `F8`。 |
| `DLAA/DepthInverted` | `0` | 已验证的深度约定，请保持 `0`。 |

jitter、运动矢量约定、DLAA 模式和锐化强度均固定为已验证值，不提供用户调整。

### 日志与故障排查

模组会在游戏目录生成 `ACOdysseyDLAA.log` 和 `ACOdysseyDLSSBridge.log`。

- 完全不生效：确认游戏 build 和 EXE 哈希，检查是否存在其他 `dinput8.dll` 模组，并更新 NVIDIA 驱动。
- 按 `F8` 后仍显示原版 TAA：检查两个日志中的 `ERROR`。Evaluate 失败后保留原版 TAA 是预期的
  故障安全行为。
- 启动异常：删除安装部分列出的六个运行文件即可；原始游戏文件没有被修改。

### 卸载

关闭游戏，只删除安装部分列出的六个运行文件。也可以删除 `ACOdysseyDLAA.log` 和
`ACOdysseyDLSSBridge.log`。不要删除属于其他模组的同名文件。

### 安装包完整性

`ACOdysseyDLAA-v1.0.0.zip` SHA-256：

```text
09c1de2e8824b99634e2a985029bd9d89b2a952e310fae1d531a8a5a85cacdee
```

压缩包内的 `SHA256SUMS.txt` 提供逐文件哈希；压缩包校验值也会作为
`ACOdysseyDLAA-v1.0.0-SHA256SUMS.txt` Release 附件提供。

### 第三方软件与免责声明

安装包根据 NVIDIA RTX SDK License 附带 NVIDIA `nvngx_dlss.dll`，并根据 BSD 2-Clause License
使用 MinHook。完整条款位于 `LICENSES/`，摘要见 `THIRD_PARTY_NOTICES.md`。

本项目是独立制作的非官方模组，与 Ubisoft、NVIDIA、Valve 及其关联公司不存在隶属、合作或背书
关系。所有产品名称和商标均归各自权利人所有。使用风险由用户自行承担。
