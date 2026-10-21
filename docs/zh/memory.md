# 内存

## 规则

**在设备上运行的代码中，每一次分配都要经过引擎。** 无论是引擎的运行时代码还是包的运行时代码，都不得使用 `new[]`、`malloc` 或 `heap_caps_*`。`Deki::Memory` 是统筹者；`Deki::Buffer<T>` 则几乎总是你需要的东西。

这不是代码风格上的偏好，并且由 [`tools/ci/check_allocations.py`](../../tools/ci/check_allocations.py) 强制执行。这条规则必须成立的三个原因：

1. **`new[]` 无法优雅地失败。** ESP-IDF 在关闭异常的情况下构建（未设置 `CONFIG_COMPILER_CXX_EXCEPTIONS`），因此分配失败会进入 `__cxa_allocate_exception` 并调用 `abort()`，开发板随即重启。没有可以添加 catch 的位置。`std::nothrow` *没有*帮助：libstdc++ 实现 nothrow 形式的方式是在 `try`/`catch` 中调用抛出形式，而 `-fno-exceptions` 已经去掉了这个 `try`/`catch`，所以它同样会中止。这是根据一个回溯诊断出来的，回溯显示 `operator new[](unsigned int, std::nothrow_t const&)` 直接调用了抛出形式。
2. **只有引擎知道内存应该放在哪里。** 这块开发板是否有外部 RAM、外部 RAM 慢多少、某个外设能否访问某个区域，这些都是平台的属性，而不是调用方的属性。自行选择区域的调用方，换到下一块开发板上就是错的。
3. **`Deki::Memory` 同时也负责统计和泄漏检查。** 统计数据、重复释放防护以及（调试构建中的）泄漏表都在它背后。绕过它的分配是不可见的。

## `Deki::Buffer<T>` - 请使用它

`<deki/providers/Buffer.h>`。一个拥有所有权的数组，通过 `Deki::Memory` 分配，并会自行释放。

```cpp
#include <deki/providers/Buffer.h>

Deki::Buffer<uint8_t> baked;

if (!baked.Allocate(width * height * 2, Deki::Memory::External))
{
    DEKI_LOG_WARNING("no room for a %dx%d bake; not drawing it", width, height);
    return false;          // carry on without the thing, do not abort
}

RenderInto(baked.Data());  // Data(), Count(), Bytes(), operator[], begin/end
```

- **只能移动。** 复制需要第二次分配，而第二次分配可能失败，复制构造函数却无法报告这一点。
- **`T` 必须是可平凡复制且可平凡析构的**，两者都由 `static_assert` 强制。这里不会构造或销毁任何元素：这些都是引擎交给 blitter 和驱动的原始像素、字形、span 和采样缓冲区。带析构函数的 `T`，其析构函数会悄无声息地永远不被调用。
- **必须指定区域**，这里和其他所有地方都一样。
- **以未改变的数量调用 `Allocate` 不做任何事**，正因如此，它可以安全地在渲染路径中每帧调用。数量不同时，会先释放旧缓冲区，而不是复制：每个调用方随后都会立即覆盖它，而同时持有两份分配，恰恰是资源受限的芯片唯一无法承受的。
- **`Allocate(0)` 是成功**，而不是失败。没有码位的稀疏字体和没有 span 的精灵都是正常的。
- **元素数量会做溢出检查。** 在设备上，`count * sizeof(T)` 以 32 位 `size_t` 计算，因此从文件或场景中读取的数量可能回绕成一个很小的分配，之后发生越界。
- `Reset()` 提前释放；`Release()` 交出原始指针，之后由调用方负责调用 `Deki::Memory::Free`。按照设计，`Release` 很少使用。

唯一没有自动化的是*内存是否到手*：在创建缓冲区的地方检查一次 `if (!buffer)`，而不是在其后的每个分支中逐层回退。

### 为什么不用 arena

帧 arena 同样可以省去失败处理，但它必须预先确定大小。一个足以容纳单次全屏渐变烘焙的 arena 会预留 150 KB，无论项目是否用到渐变；在只有几百 KB 堆的芯片上，这相当于为一个可能并不存在的功能占用了一半内存。arena 在有余量的地方才划算。这里的大多数分配都是长期存在的，并且本来就归某个对象所有：字形表、像素数据、粒子列、帧缓冲区，这正是 RAII 所服务的，而 arena 不是。

## 区域 - 每次都由你选择

`<deki/providers/Memory.h>`。**没有默认值，也不做任何推断。** 每个分配调用都要指定一个区域，省略它就无法编译。

| 区域 | 位置 |
| --- | --- |
| `Deki::Memory::Internal` | 内部 RAM。速度快，也是稀缺的那一个 |
| `Deki::Memory::External` | 开发板有 PSRAM 时使用 PSRAM。以 MB 计，但明显更慢：它要通过 SPI 访问 |

`Deki::Memory::AllocateDma(bytes, region)` 额外加上一个约束：DMA 引擎必须能够读取该内存块。这与放置位置是正交的：无论开发板将帧缓冲区放在内部还是 PSRAM 中，它都必须可被访问，因此它是一个单独的入口点，而不是第三个区域。

### 区域就是一个名称，所以包可以添加自己的区域

引擎自带两个区域。这个集合是开放的：

```cpp
// in a board-support package - no engine change, no registration call
namespace DekiEsp32
{
inline constexpr Deki::Memory::Region Rtc{"esp32.rtc"};
}

battery.Allocate(count, DekiEsp32::Rtc);
```

包将自己的区域放在**自己的命名空间**中，而不是放在 `Deki::Memory` 中。它本来也做不到（类是封闭的），而这正是关键所在。两个都想要 `Rtc` 常量的开发板包不会冲突，因为语言本身会把它们分开。常量放在哪里从来都不重要：区域的身份是其名称的哈希值，所以把它从一个命名空间移到另一个命名空间不会带来任何变化。

`Region()` 在编译期用引擎其余部分使用的同一个 FNV-1a 对名称进行哈希，因此区域是一个常量表达式，提供者可以对它使用 switch。

**两个包命名了同一个区域，就表示同一个区域**。正是这样，两个开发板包无需协调就能在 `"rtc"` 上达成一致，就像两个包都要求 `spi` 能力一样。无法应对的情况是两个包用同一个词表示*不同*的东西，所以对不是你发明的名称要加上限定：`particles.scratch`、`esp32.rtc`。由于分配头只存储 id，调试构建会报告哈希值相同的两个不同名称。

### 提供区域是开发板的职责

包可以*命名*一个区域；只有开发板的 `IMemoryProvider` 才能提供它。不认识 `esp32.rtc` 的提供者会返回 nullptr，失败信息中会写出该区域的名称：

```
Failed to allocate 64 bytes in 'esp32.rtc' for 'void Battery::Save()'
at Battery.cpp:88 (0 bytes free there)
```

它不会被替换为其他区域。对 `Rtc` 来说这一点尤其重要：它的意义在于在深度睡眠后保留数据，因此如果悄悄换成普通 RAM，数据就会恰好在本应保留的那一刻丢失。

### 区域名称就是能力名称

目标将其拥有的区域作为能力公布，前缀为 `memory:`：

| 能力 | 来源 |
| --- | --- |
| `memory:internal` | 每个平台 - 推导得出 |
| `memory:external` | 平台的 `psramSize` - 推导得出 |
| `memory:esp32.rtc` | 开发板的 `provides`，因为只有它知道 |

因此，需要某个区域的包只需声明它，构建就会在任何东西运行之前，按名称拒绝缺少该区域的目标：

```json
"platformRequires": ["memory:esp32.rtc"]
```

这两部分不会产生偏差，因为 `Region("esp32.rtc")` 和 `"memory:esp32.rtc"` 是同一个标记加上前缀，而不是由某人保持同步的两份列表。能力名称使用小写字母以及 `.`、`-`、`:` 和 `_`；声明 `"WiFi"` 的平台会在加载时收到警告，因为精确字符串匹配会悄无声息地永远不生效。

### 放置只解析一次。没有回退。

如果所选区域已满，分配会**失败并返回 null**。它不会在另一个区域中重试。这是有意为之：

- 悄悄把一个 `Hot` 缓冲区移到外部 RAM，会把放置错误变成谁也说不清原因的掉帧。
- 悄悄把一个 `Buffer` 移到内部 RAM，会耗尽下一次 `Hot` 分配所需的堆，导致失败出现在毫不相干的地方。

失败时会记录分配的内容、大小、区域，以及未尝试回退。每个调用方本来就必须处理 null。

没有外部 RAM 的开发板会把所有内容解析到内部。这不是回退；那是唯一存在的区域。

## API 的其余部分

`Deki::Memory`（`<deki/providers/Memory.h>`），用于 `Buffer<T>` 无法覆盖的情况：

```cpp
void* Allocate(size_t size, Memory::Region region, AllocContext ctx = {});
void* AllocateDma(size_t size, Memory::Region region, AllocContext ctx = {});
T*    AllocateArray<T>(size_t count, Memory::Region region, AllocContext ctx = {});
void  Free(void* ptr, const char* context = nullptr);
```

- 内存在返回前会被**清零**，因此缓冲区一开始绝不会充满杂乱数据，调用方也不必自己写清零循环。
- `Free` 通过读取紧挨在用户指针之前的头部来判断内存块来自哪个区域，因此调用方无需选择。该头部还带有一个魔数，能把重复释放或外来指针变成一条记录下来的错误，而不是堆损坏。
- **为分配命名是可选的。** 参见下文的 `AllocContext`。
- 失败时会报告所尝试的区域中还有多少空闲内存，从而区分“这个资源对这块开发板来说太大”和“某处占着本应释放的内存”。
- `Memory::Initialize` 在启动时记录一次空闲的内部和外部 RAM，这样项目就可以根据真实数字来确定规模，而不是等到第一条内存不足的日志。
- `AllocateInternal` / `FreeInternal` 绕过放置决策，是为平台层准备的。不要在组件中使用它们。

### 查明内存去了哪里

```cpp
Deki::Memory::LogStats("after level load");
// Memory at after level load: 48216 bytes allocated (48216 internal, 0 external);
//                             237096 free internal
```

- `LogStats(label)` 一次调用就能回答“我用了多少内存？”。在调试构建中，它还会报告当前和峰值的分配数量。不调用就没有任何开销。
- `GetStats()` 以结构体形式返回同样的数字；`GetAvailableInternal()` 和 `GetAvailablePSRAM()` 给出空闲的部分。`IsPSRAMAvailable()` 表示是否存在外部区域。这些都不意味着你可以自行放置内存：它们用于确定项目规模，以及查找是什么占着本应释放的内存。

## `AllocContext` - 标签自动生成

每个分配调用都接受一个 `AllocContext`，而几乎没有人需要写它：

```cpp
posX.Allocate(capacity, Deki::Memory::Internal);
```

在调试构建中，默认参数会捕获调用位置，因此失败信息如下：

```
Failed to allocate 153600 bytes in internal RAM for
'void ParticlePool::SetCapacity(size_t)' at ParticlePool.h:47
(126556 bytes free there)
```

没有需要传入的名称，也没有传入名称的方法。手写的标签是同一次分配的第二个身份，必须有人让它与第一个保持一致，而编译器早已知道答案。同一个函数中的两次分配通过行号区分：

```
'void ParticlePool::SetCapacity(size_t)' at ParticlePool.h:47
'void ParticlePool::SetCapacity(size_t)' at ParticlePool.h:48
```

**它指明的是调用方，而不是引擎。** `Buffer<T>::Allocate` 和 `Memory::Allocate` 都接受上下文并将其转发，而不是重新使用默认值，因此记录的位置是*你*写下调用的地方，而不是 `Buffer.h`。有一个测试专门断言这一点，因为如果搞错了，引擎中的每次分配都会报告一个看似合理的文件，结果是自信满满却毫无用处。

**不复制任何内容。** 每个成员都指向一个具有静态存储期的字符串字面量，这也是调试泄漏表可以持有该指针的原因。

### 去除它

捕获在每种构建中都是开启的。它在 **ESP32-S3 固件上占用 4,272 字节，即 1.5 MB 镜像的 0.28%**，每个调用位置约 40 字节，便宜到不值得考虑。

对于需要收回这部分闪存的构建，定义 `DEKI_STRIP_ALLOC_CONTEXT`。`AllocContext` 会变成一个空结构体，二进制文件中不会出现任何路径或函数名，失败时只报告它试图做什么，而不声称来源：

```
Failed to allocate 153600 bytes in internal RAM (126556 bytes free there)
```

注意，去除和未去除的构建布局不同，因此引擎和调用它的包必须保持一致，这与 `MemoryStats` 已有的约束相同。

## `Deki::Vector<T>` - 数量在运行时变化时

`<deki/providers/Vector.h>`。`Buffer<T>` 适用于分配时就已知的数量。当数量会变化且没有固定上限时，在设备上 `std::vector` 就是错误的选择：`push_back` 会调用 `operator new`，而在关闭异常的情况下它无法报告内存耗尽，于是中止，开发板重启。

`Deki::Vector` 就是这种形态，只改了在这里唯一重要的一点：所有可能分配的操作都返回 `bool`：

```cpp
Deki::Vector<Enemy> enemies(Deki::Memory::Internal);

if (!enemies.Reserve(32))            // out of memory, said once
    return;

if (!enemies.PushBack(spawned))      // and per growth, if it has to grow
    DEKI_LOG_WARNING("dropped a spawn");
```

- **区域是构造函数参数**，因此读者在声明处就能知道这个列表占用的是哪种内存，并且每次增长都落在同一个地方。没有默认构造函数：会增长的 vector 必须知道新的内存块放在哪里，而引擎不会替它决定。

- **预先调用 `Reserve` 能让随后的 push 不会失败。** 只需检查一次，而不是每个元素检查一次；这是在设备上值得采用的写法。
- 增长**从首块 4 个元素开始倍增**，而不是 16 个：设备项目中有许多短列表，每个都使用较大的默认值，加起来比列表本身占用的还多。
- **增长失败时，已存储的内容全部保持完好**，因此丢弃新元素的调用方仍然拥有一个可用的 vector。
- `RemoveUnordered(i)` 是 O(1) 的，用最后一个元素填补空位，与 `ParticlePool::KillSwap` 使用的技巧相同，也正是存活实体列表所需要的。`Clear()` 保留内存以便下一帧重新填充；`Reset()` 则释放它。
- 对 `T` 的约束与 `Buffer` 相同：可平凡复制且可平凡析构。增长时用 `memcpy` 移动元素。

优先使用 `Buffer`。这里大多数“动态”数据最后都会发现有一个项目早已知道的上限，而固定分配不会像反复增长那样让小堆产生碎片。

### `std::vector`、`std::string` 和普通的 `new`

这些会调用 `operator new`，`Deki::Buffer<T>` 无法替代它。在设备构建中，引擎会替换这些运算符本身（[`src/providers/OperatorNew.cpp`](../../deki-engine/src/providers/OperatorNew.cpp)），这会带来两处变化：

- **分配失败时会说明情况。** 它会记录想要分配的大小，然后中止，因为在关闭异常的情况下，会抛出的 `operator new` 别无选择。“Out of memory asking for 40960 bytes”是一条诊断；它所取代的重启则不是。
- **`std::nothrow` 可以正常工作。** `new (std::nothrow) T[n]` 会返回 `nullptr` 而不是中止，因此想自行处理内存耗尽的代码可以做到。没有这个替换时它会中止，因为 libstdc++ 实现 nothrow 形式的方式是在已被去除的 `try`/`catch` 中调用抛出形式。

这些**确实**经过引擎：`operator new` 会调用 `IMemoryProvider::AllocateRaw`，因此开发板能看到设备上的每一次分配，并可以用自己的堆来承担全部分配。

但它*不是* `Memory::Allocate`。如果是的话，每个超出小字符串缓冲区的 `std::string` 前面都会多一个 16 字节的头部，vector 即将覆盖的内存会被 memset，小内存块也要付出 ESP32 提供者的 16 字节 SIMD 对齐开销，而 ESP-IDF 是通过多分配来实现对齐分配的，所以这比头部浪费得更多。`AllocateRaw` 没有这些：自然对齐、没有头部，就是堆返回的那个指针。

实测，160 万次成对的 48 字节分配，引擎为 `-O2`：

| | ns/op |
|---|---|
| `malloc` + `free` | 47-52 |
| 经由提供者 | 46-51 - 无法区分 |
| 经由提供者，并启用 `DEKI_MEMORY_TRACK_RAW` | 63-71 (+35%) |

所以转发是免费的，并且始终开启；**统计字节数则需要主动开启**，因为没有头部时，内存块大小必须从堆中获取，而在 Windows 上这意味着在锁下由 `_msize` 调用 `HeapSize`。在追查内存去向时用 `DEKI_MEMORY_TRACK_RAW` 构建，`Memory::LogStats` 就会报告容器占用了多少；没有它时，那一行会说明这一点，而不是打印一个误导性的零。

随头部一起放弃的还有它的魔数，因此**容器内存的重复释放和以前一样无法诊断**，就像这条路径还是普通 `malloc` 的时候。凡是引擎能够描述的分配都经过 `Allocate`，并保留这项防护。

这里有意不决定放置位置。像已移除的 `Auto` 规则那样按大小分流，会悄悄把一个大的 `std::vector` 移到 PSRAM 中，让热循环变慢，而原因在代码中完全看不出来。想要外部内存的调用方会指定 `Deki::Memory::External` 并使用 `Deki::Buffer`；`operator new` 无从得知，所以它不去猜。

它只在 `__cpp_exceptions` 未定义的地方编译，也就是恰好存在中止问题的那些构建。主机和编辑器构建保留标准运算符。超对齐形式（`align_val_t`）留给 libstdc++，以便它们保持成对匹配。

所以：对于运行时会变化的数量，使用 `Deki::Vector<T>` 并处理 `false`。对于大小来自文件、场景或屏幕的任何东西，使用 `Deki::Buffer<T>`。`std::vector` 仍然适用于编辑器和工具代码；而在设备上，它的内存耗尽现在是一次有日志记录的中止，而不是无声的中止。

## 平台层：`IMemoryProvider`

`<deki/providers/IMemoryProvider.h>`。平台提供一个实现，并在 `Memory::Initialize()` 之前交给引擎：

```cpp
Deki::Memory::SetBackend(new MyBoardMemoryProvider());  // takes ownership
Deki::Memory::Initialize();
```

该接口报告外部 RAM（`HasExternalRAM`、总量、空闲量），并从各个区域分配：`AllocateExternal` / `FreeExternal`、`AllocateInternal` / `FreeInternal` 以及 `AllocateDMA`。

**每个与放置相关的虚函数都是纯虚函数，必须全部实现。** 提供者要说明每个区域在其平台上的含义，即使答案是“在这里和其他区域一样”。在 `AllocateDMA` 中写 `return malloc(size);` 的桌面提供者，就做出了一个可以阅读和检查的声明。

`AllocateDMA` 是最重要的那个。如果提供者可以从 `AllocateInternal` 继承它，那么在两者不同的任何芯片上，都会把显示外设无法访问的内存交给它：悄无声息的数据损坏，构建中也没有任何东西会指出这一点。

出于同样的原因，`GetAvailableInternalRAM` 也是必须实现的：随意返回 0 的提供者会让启动日志报告开发板内存已耗尽。

纯虚函数与已实现的虚函数占用相同的 vtable 槽位，因此这不需要升级 ABI 版本。它只是让提供者在做出决定之前无法编译。

`AllocateRaw` / `FreeRaw` / `GetRawBlockSize` 是第四对函数外加一个查询：全局 `operator new` 所经由的、按自然对齐分配的普通分配器，这样开发板的堆既能为引擎放置的一切服务，也能为 `std::vector` 和标量 `new` 服务。它们默认使用 `malloc`/`free`，在平台无法回答时 `GetRawBlockSize` 返回 0，这只会影响 `LogStats` 的准确性。这一对要么都重写，要么都不重写。

`AllocateDMA` 与 `AllocateInternal` 分开，是因为在 ESP32-S3 上大部分内部 RAM 可被 DMA 访问，但并非全部；显示外设读取不可访问的帧缓冲区时，得到的是悄无声息的数据损坏，而不是错误。ESP32 提供者会显式请求 `MALLOC_CAP_DMA`。重写 `AllocateInternal`/`FreeInternal` 中任意一个的人必须两个都重写：这一对必须匹配（ESP32 提供者使用 16 字节对齐的 `heap_caps_aligned_alloc`，这也让 blit 调度器能够启用 S3 的 SIMD 内核，以及 `heap_caps_free`）。

**`IMemoryProvider` 是包 ABI 的一部分**，因此它的虚函数由 [`tools/ci/check_abi.py`](../../tools/ci/check_abi.py) 锁定。添加、删除或重新排序其中任何一个都会移动 vtable 槽位，并且需要在同一个提交中提升 `kDekiReflectionAbiVersion`。

## lint 检查

```bash
python tools/ci/check_allocations.py            # packages beside the repo
python tools/ci/check_allocations.py --verbose  # also list files and waivers
```

在运行时代码中发现 `new[]`、`delete[]`、`malloc`/`calloc`/`realloc` 和 `heap_caps_*` 时失败，**既包括引擎自身的代码，也包括每个包的代码**。引擎中的一次分配会影响每块开发板上的每个项目，而包中的分配只影响安装了它的项目，所以如果说有什么区别，引擎才是更重要的那一半；这个 lint 的第一个版本没有包括引擎，结果立刻证明了这一点：`ProjectSettings` 一直在用裸 `realloc` 保存其设备端的设置数据块。

跳过的部分：`editor/`、`tests/`、`generated/`、`third_party/`，以及引擎的 `platforms/editor` 和 `platforms/desktop` 层，这些都运行在主机上，在那里异常是开启的，堆也是桌面系统的堆。按路径豁免的部分（因为它们是技术栈的最底层，而不是被允许违反规则的代码）：`providers/Memory.cpp`、`providers/IMemoryProvider.h`、所有 `*MemoryProvider.*`，以及 `providers/OperatorNew.cpp`，后者必须定义 `operator new[]`，这样其他任何地方都不需要定义它。

注释和字符串字面量在匹配前会被清空，因此解释这条规则的注释不会触发它。

确实无法避免的情况可以使用豁免，但**必须写明理由**；没有理由的豁免不算数，这样 diff 中总能看出哪些例外是经过论证的：

```cpp
void* p = malloc(n);  // deki-alloc-ok: the C API frees this itself
```

它已经证明了自己的价值：它在 `Standard2DRenderer` 中发现了一个对 `Deki::Memory` 缓冲区使用 `delete[]` 的地方，而之前的人工排查漏掉了它。
