# 平台

**平台**是项目构建的一个目标：一块开发板、桌面模拟器或一台游戏机。本页介绍平台由什么组成、平台从哪里来、包如何附带开发板，以及项目开始使用某块开发板之后会发生什么。

## 平台是什么

项目 `platforms/` 下的一个目录，以平台的 id 命名：

```
<project>/platforms/<id>/<id>.json    the platform
<project>/platforms/<id>/boot.scene   the scene a build of it boots
<project>/platforms/<id>/origin.json  only if it came from a package (see below)
```

JSON 描述编辑器需要知道的内容，而不涉及如何构建：

| 键 | 含义 |
| --- | --- |
| `id`, `displayName` | id 同时也是目录名。 |
| `framework` | 由哪个构建后端来构建它（`espidf`、`native`、...）。没有默认值：未指定的平台就没有后端，构建时会提示这一点。 |
| `frameworkOptions` | 该后端自己的设置：芯片、闪存大小、sdkconfig 行、引脚。编辑器只负责携带而不读取它们；由后端在构建时进行校验。 |
| `family` | 仅用于描述。会成为能力 `family:<x>`。 |
| `screenWidth`, `screenHeight`, `colorFormat` | 该目标的屏幕。编辑器会预览它，桌面模拟器会打开该尺寸的窗口，设为“自动”的图像会以 `colorFormat` 为它存储。设备以其显示屏自身的尺寸和格式进行渲染；参见 [screens.md](screens.md)。 |
| `externalMemorySize` | 内部堆之外的内存字节数（在微控制器上即 PSRAM）。为包提供 `memory:external`。 |
| `provides` | 该目标可以为包提供的东西：`wifi`、`spi`、`desktop-os`、... |
| `requiresPackages` | 无论场景使用什么，该目标始终需要的包。通常是它的硬件抽象层。 |
| `defines`, `cFlags`, `cxxFlags`, `linkFlags` | 游戏自身代码的编译器设置。 |
| `bootScene` | 引导场景的路径，相对于项目。 |
| `assetStorage` | 构建将游戏资源保存在哪里：`internal`（默认）或 `external`。见下文。 |

**引导场景**包含一个 `Deki::PlatformSetupComponent`，其 `setupComponents` 列表按顺序列出启动硬件的配置步骤：电源引脚、显示屏、I2C 总线、触摸。每个条目为 `{id, typeId}`：持有该步骤的子对象的 id，以及该步骤限定类名的哈希值（`Deki::HashString`）。`typeId` 错误不会导致失败；该步骤只是悄无声息地永远不会运行。编辑器测试中的 `ShippedPlatformTemplates` 会检查各个包附带的每个引导场景。

## 资源放在哪里 {#where-the-assets-go}

构建将游戏资源保存在以下两个位置之一：

- **内部存储**，默认选项：设备的一部分。资源由构建携带，放在引导场景旁边的 `assets/` 中（运行时为 `F:/assets/`）。在 ESP32 上就是闪存：数据分区占用芯片的其余部分（在 16 MB 的开发板上约为 13 MB），如果游戏放不下，构建就会失败。桌面模拟器和 Tactility 应用的工作方式相同。
- **外部存储**：可移除的存储，即 SD 卡。构建只携带引导场景和 `project_data.bin`；**SD 卡部署**面板会将资源复制到卡上，卡的存储包在挂载后从卡中加载它们（`S:/`）。

只有 `provides` 中包含 `external-storage` 的平台才能选择外部存储；平台编辑器只对这些平台显示该选项（**存储 > 资源存放位置**）。只有游戏大到闪存放不下时才选择它：使用外部存储时如果卡槽中没有卡，游戏就没有任何资源。LilyGO T-Deck Plus 可以选择；除非另行指定，它会将资源保存在闪存中。

`project_data.bin` 会记录这一选择，以便引擎知道去哪里查找。

## 模拟开发板

如果开发板的后端带有模拟器（ESP32 或 ESP32-S3 使用 QEMU），就可以将其构建为在模拟器中运行：在构建面板中勾选 **在 QEMU 中模拟**，然后点击“构建”和 **运行**。在命令行中：

```
DekiEditor --build-firmware --deploy --simulate --platform lilygo_t_deck_plus <project>
```

模拟构建保留开发板引导场景的每一个步骤，并在其后加上模拟器自己的步骤（QEMU 屏幕），这些都保存在其构建目录下的一个副本中（`generated/build/<platform>_qemu/simulation/boot.scene`）。如果模拟器需要以其他方式运行某个步骤，副本中会注明：通过 SPI 连接的 SD 卡会改为接在 QEMU 的 SD 主机上。构建日志会列出每一处改动。平台的 `boot.scene`、它的常规构建以及烧录到开发板上的固件都不会被更改，模拟器需要的任何东西也都不会编译进去。

模拟的开发板带有一张 SD 卡，每次构建时在 `simulation/sd_card` 中重新生成：对于将资源保存在卡上的目标，卡中是 SD 卡部署面板会复制的那些资源；对于将资源保存在内部的目标，卡是空的。

后端通过 `ITargetBuilder::SupportsSimulation`、`GetSimulatorName` 和 `MakeSimulationBootScene`（配合 `deki-editor/build/SimulationBootScene.h` 中的 `AddSimulationSteps`）提供此功能，其余部分则读取 `BuildOptions::simulate` 来处理：它自己的构建目录、SD 卡以及部署时要做的事。

## 平台从哪里来

构建面板的目标列表有三个来源，后面的覆盖前面的：

1. `~/.deki-editor/board-packs/` 下的**开发板包**。
2. 项目中安装的**包**：每个 `<package>/platforms/<id>/`。
3. **项目自己的** `platforms/`。

安装或移除包会立即更新该列表。

## 在包中附带开发板

在包中为每块开发板放一个目录：

```
deki-lilygo-boards/
├── package.json
└── platforms/
    ├── lilygo_t_deck_plus/
    │   ├── lilygo_t_deck_plus.json
    │   └── boot.scene
    └── lilygo_t_display_s3_amoled/
        ├── lilygo_t_display_s3_amoled.json
        └── boot.scene
```

这就是全部机制。包不需要任何代码：不需要 `package.cmake`，也不需要入口点。它的 `package.json` 应在 `requires` 下列出其开发板的引导场景所用的包，这样安装开发板时也会一并安装这些包；每个平台还应在 `requiresPackages` 中再次列出它的硬件抽象层。

会被检查的规则：

- **目录名就是平台 id**，JSON 是其中的 `<id>.json`。它必须是一个简单的名称；任何看起来像路径的名称都会被跳过。
- **两个已安装的包提供同一个 id 是错误**，在有人选择它时报告。请为 id 加上厂商前缀（`lilygo_...`）。
- **`defines` 不得指定 `DEKI_PACKAGE_*` 或 `DEKI_FEATURE_*`。** 构建中有哪些包，由解析器决定。始终需要某个包的开发板应在 `requiresPackages` 中声明。
- **`bootScene` 相对于项目**，而不是相对于包：`platforms/<id>/boot.scene`。被构建的是采用后的副本。

### 需要其他包的功能

包的功能可以在该功能的 `requires` 中指定它单独需要的包：

```json
{ "id": "trackball", "componentTypes": ["TrackballComponent"],
  "requires": ["deki-gpio"] }
```

这些包不会随该包一起安装。由使用该功能的开发板或项目引入它们：`deki-lilygo-boards` 需要 `deki-i2c` 和 `deki-gpio`，因为 T-Deck 有 I2C 总线和引脚，正是这一点让 `deki-input` 的键盘和轨迹球得以构建。安装了 `deki-input` 的桌面项目则两者都不会得到。

只有当某个目标使用了该功能时，构建才会引入该功能的包。正在使用但缺少所需包的功能，会被构建为一个在日志中记录需要安装什么的步骤，并且构建会发出警告。功能的代码用 `__has_include` 检查该包，因此无论哪种情况都能编译。

`deki-lilygo-boards` 是可供参照的示例。`deki-desktop-integration` 以同样的方式附带模拟器，`deki-tactility-integration` 则附带它的两个目标。

## 采用：项目获得一份副本

选择某个包的平台（在构建面板中、使用 `set_active_platform`，或为它进行构建）会将其目录复制到项目的 `platforms/` 中。此后它就是项目的文件：在平台编辑器中编辑它，像打开任何场景一样打开它的引导场景，提交它。项目绝不会从包内部进行构建。

项目已经定义的平台绝不会被采用操作覆盖，该目录中已有的文件也会保留。

## 来源：编辑、更新、恢复

采用操作还会在平台旁边写入 `origin.json`。它记录包的名称，并保存**采用时**的模板：平台 JSON，以及其他每个文件的哈希值。请将它与其余文件一起提交。

当采用时的内容、项目当前的内容以及包当前提供的内容都已知时，构建面板会在目标列表下方显示：

```
From package deki-lilygo-boards 0.17.0 - modified - update available
[Update] [Restore defaults]
```

- **已修改**：项目的副本与采用时的内容不同。将鼠标悬停在上面可查看差异。
- **有可用更新**：包提供的内容与采用时的不同。
- **更新** 会接受包所做的更改，并保留项目所做的更改。平台设置逐项合并，后端的 `frameworkOptions` 也包括在内；其他文件（包括引导场景）则整体接受或整体保留。双方都更改了同一处时，保留项目的版本并列出该项，同时包的版本成为新的基准，因此这个问题只会询问一次。
- **恢复默认** 在确认后用包当前的版本替换副本。只有项目才有的文件（资源附属文件、项目自己的场景）会被保留。

以下情况不算作修改：编辑器以自己的格式重新保存 JSON（较旧的文件将后端设置放在顶层；保存时会将它们嵌套到 `frameworkOptions` 下），以及被 git 更改的换行符。

在开始记录来源之前就已采用的平台，会在下次使用时获得记录，前提是它仍与包提供的内容一致。如果不一致，就无从判断那是谁做的更改，因此不会凭空创建基准：它会显示为*与包中的版本不同*，可以恢复，但不能更新。

如果包被移除，平台会保留并且仍然可以构建。在包重新安装之前，该行会显示*（未安装）*。

删除一个已采用的平台会让它重新出现在可选列表中；再次选择它会复制包的当前版本。

## 从命令行使用

| 工具 | 作用 |
| --- | --- |
| `list_platforms`（或 `--list-platforms`） | 列出每个目标，来自包的目标带有 `origin`：`package`、`modified`、`updateAvailable`、`canUpdate`、`canRestore`。 |
| `set_active_platform` | 选择项目的默认目标，必要时采用它。 |
| `create_platform`, `set_platform_provides` | 创建或更改项目自己的平台。`set_platform_provides` 接受 `asset_storage` 和 `cxx_flags`。 |
| `update_platform` | 从包更新。返回 `taken` 和 `kept`。 |
| `restore_platform` | 恢复包的版本。返回 `taken`。 |

```
DekiEditor --tool update_platform --args '{"platform_id":"lilygo_t_deck_plus"}' <project>
```

## 在代码中

`deki-editor/build/PlatformConfig.h` 是平台本身及其注册表。`deki-editor/build/PlatformTemplates.h` 包含本页中与包有关的所有内容：查找可选平台、采用、来源记录、更新和恢复。两者都在编辑器的 SDK 中，因此包的构建后端可以使用它们。
