# 项目结构

Deki 项目在磁盘上是什么样子、编辑器会生成什么，以及生成的状态出问题时如何恢复。

## 结构

```
<project>/
├── deki.json                    project identity and entry points
├── deki-packages.json           declared package dependencies
├── deki-packages-lock.json      resolved versions
│
├── src/                         gameplay code
├── assets/                      sprites, fonts, scenes, data, rendering
├── packages/                    installed package sources
├── platforms/                   build targets
│
├── settings/                    project settings
│
└── generated/                   everything the editor generates
    ├── runtime/                 package DLLs the editor loads
    ├── build/                   CMake/Ninja tree
    ├── cache/                   asset cache
    ├── shadow/                  hot-reload copies of runtime/
    └── CMakeLists.txt           regenerated on open/build
```

`platforms/` 为每个构建目标保存一个目录，包括从包中复制过来的目标：参见 [platforms.md](platforms.md)。

`generated/` 下的所有内容都是可丢弃的。这就是项目的 `.gitignore` 只有一行而不是一份清单的原因：清单会逐渐与实际脱节，而且确实发生过：它曾列出一个没有任何东西会创建的 `build_refl/`，却把一个受版本控制的 `settings/` 文件夹隐藏了。

## 可移植性

项目目录可以复制到另一台机器上并直接打开。项目中不记录任何绝对路径：

- 路径在运行时根据项目根目录推导得出，参见 `src/core/ProjectPaths.h`，这是项目结构的唯一定义。
- 生成的 `CMakeLists.txt` 根据自身所在位置推导出 `DEKI_PROJECT_ROOT`，并在配置时从编辑器获取引擎和编辑器的位置（`-DDEKI_ENGINE_PATH` / `-DDEKI_EDITOR_DIR`，由 `EditorSettings::GetCMakeLocationArgs` 生成）。以前这些位置是写死在文件中的，一旦编辑器的安装位置变化就会出错。
- 提交到 `src/generated/` 下的反射输出使用相对源路径。

移动**编辑器**同样是安全的：引擎路径在每次调用时都根据正在运行的可执行文件解析（`EditorSettings::GetEnginePath`），从不保存。下载的编辑器会在其旁边的 `sdk/` 文件夹中找到它们（`EditorSettings::GetSdkDir`）；从构建目录运行的编辑器则会在包含该构建文件夹的源代码检出中找到它们。

## 恢复

使用 **编辑 → 维护**。这些操作按开销从低到高排列；只有开销低的操作无效时，才使用下一个。

| 症状 | 操作 | 开销 |
|---|---|---|
| 某个包已过时，或已卸载的包仍被加载 | 重置包副本 | 几秒钟 - 下次加载时重新复制 |
| 导入后资源看起来不对或已过时 | 清除资源缓存 | 资源重新导入 |
| 链接器或 CMake 混乱、配置失败 | 清理构建 | 完全重新编译 |
| 从头开始 | 完全清理 | 重新构建 + 重新导入 |

每项操作都对应 `generated/` 下的一个目录。手动删除这些目录效果相同；提供这个菜单是为了不必记住项目结构。运行模式或构建正在进行时，这些操作会被禁用，因为删除已映射的 DLL 会中途失败，留下比开始时更糟的状态。

## 为什么 shadow/ 与 runtime/ 并列

包是从 `generated/shadow/` 中的副本加载的，而不是直接从 `generated/runtime/` 加载。Windows 会锁定已映射的 DLL，因此加载副本可以让原始文件保持可写，使编辑器能够重新构建当前已加载的包。

`shadow/` 有意放在 `runtime/` 旁边，而不是其内部：`runtime/` 是复制的*来源*，把目标嵌套在来源中，意味着加载器会枚举到它自己的副本。`shadow/owner.pid` 记录所属进程，这样崩溃实例的残留文件会被自动回收，而不需要手动删除。

## 结构版本

`deki.json` 带有 `"structure"`。编辑器在加载时将其与 `ProjectPaths::kStructureVersion` 比较，运行一次结构迁移，然后记录新版本。因此，尚未迁移的项目总能描述自身，而不必让每次迁移都从目录的形态推断状态。
