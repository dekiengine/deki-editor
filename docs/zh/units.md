# 单位

**这是唯一记录单位的地方。** 只要本引擎中的某个数字带有单位，它就在本页上。

## 规则

**存储的值是国际单位制（SI）。始终如此。** 米、弧度、秒、千克、摄氏度。组件、场景文件或包 API 中的 `float` 都使用本页给出的单位：没有例外，也不因项目而异。

换算只发生在一个边界上：**显示**。检视器可以用像素显示距离、用度显示角度、用华氏度显示温度。进入场景文件和数学运算的都是 SI。

这不是代码风格上的偏好。没有它，有三件事会出问题：

1. **序列化的场景会产生歧义。** 一个 `width: 4.0`，取决于是谁保存的，可能是米也可能是像素，这样就无法可靠地读回。
2. **数学运算会悄无声息地混用比例。** 把以 px/s 表示的速度加到以米表示的位置上，能够编译，并得出一个看似合理的错误结果。
3. **包之间无法互操作。** `deki-2d` 和物理包必须在不互相查阅的情况下，对一个浮点数的含义达成一致。

## 存储的内容

| 物理量 | 存储为 | `PhysicalUnit` | 检视器可显示为 |
| --- | --- | --- | --- |
| 距离 | **米** | `Distance` | px（× `pixelsPerMeter`）或 m |
| 速度 | **米每秒** | `Velocity` | px/s 或 m/s |
| 加速度 | **米每二次方秒** | `Acceleration` | px/s² 或 m/s² |
| 角度 | **弧度** | `Angle` | 度，始终如此 |
| 角速度 | **弧度每秒** | `AngularVelocity` | 度/s，始终如此 |
| 温度 | **摄氏度** | `Temperature` | °C 或 °F |
| 模拟时间 | **秒**，`float` 类型 | - | - |
| 持续时间、超时 | **毫秒**，整数类型 | - | - |
| 频率 | **赫兹** | - | - |
| 数据大小 | **字节** | - | - |
| 质量 | **千克** | - | 保留；目前尚无任何地方使用 |

`PhysicalUnit` 是一个**显示提示**，而不是关于存储的声明。省略它意味着检视器只显示一个数字；它从不改变值的含义。

### 为什么时间有两种约定

两者都是有意为之，并且**类型会告诉你用的是哪一种**。

**以秒为单位的 `float`** 是模拟增量：一帧推进多少世界时间。秒能让数值保持在 1.0 附近，那里的浮点精度最好，而且积分步骤乘的正是它。

**以毫秒为单位的整数**是挂钟时间：超时、动画帧时长、时间戳。它是精确的，用于比较而不是积分，并且通常要经过使用毫秒的平台 API。

如果名称以 `Ms` 结尾，它就是一个以毫秒计的整数。不带后缀的时间 `float` 就是秒。

## 数据手册不使用 SI，这没有问题

传感器数据手册使用 g 和度每秒。返回这些单位的 API 并没有错，错的是从名称中没有说明这一点的方法返回它们。`IDekiIMU` 展示了这种模式：

```cpp
// SI, and the only virtual: this is what the engine stores
virtual DekiVec3f ReadGyroRadiansPerSecond() const = 0;

// the datasheet's unit, derived and NOT virtual, so a driver cannot make
// the two disagree
DekiVec3f ReadGyroDegreesPerSecond() const;
```

两个名称都带有各自的单位，因此都不会被误读。对照芯片 ±245 dps 的指标进行调试时，使用数据手册的写法；任何对读数进行积分的代码都使用 SI。换算只存在于一处，即驱动的比例常量中，并且数据手册的数值在表达式中仍然清晰可见：

```cpp
float m_GyroScale = (245.0f / 32768.0f) * Deki::Math::kDegToRad;  // LSB -> rad/s
```

`ReadAccelMetersPerSecondSquared()` 和 `ReadAccelG()` 也是如此。

**如果你添加传感器 API，请在方法名中写明单位。** 返回一个裸向量的 `ReadGyro()` 正是这条规则要防止的东西：它的文档写的是 dps，而引擎中其他所有角度值都是弧度，在错误的假设下送入旋转的读数会差 57.3×。

## 显示换算

全部位于 `deki-editor/src/core/Units.h`。只有一个文件，因此添加单位只需修改一处：

- `GetUnitDisplay(unit)` - 乘数、偏移量、printf 格式、拖动速度。
- `ToDisplay(stored, ud)` / `FromDisplay(shown, ud)` - 一对互逆函数，放在一起。当每次换算都只是一个乘法时，调用方可以在行内求逆；有了华氏度的偏移量后，手写的 `shown / multiplier` 会差 32 度，而且看起来仍然合理。
- `CanonicalUnitSymbol(unit)` - 值*存储*时使用的 SI 符号，用于机器可读的输出。不受显示偏好影响：在元数据转储中写入 "px" 会让转储变成谎言。
- `SnapMetersToPixel` / `ShouldSnapToPixel` - 仅用于距离，且仅在像素模式下。

控制换算的项目设置，全部仅影响显示：

| 设置 | 效果 |
| --- | --- |
| `inspectorUnit` | 距离类单位使用 `Pixels` 或 `Meters` |
| `pixelsPerMeter` | px↔m 换算系数，默认为 16 |
| `temperatureUnit` | `Celsius` 或 `Fahrenheit` |

更改其中任何一项，都**不会**改变磁盘上或内存中的任何内容。

## 为属性添加单位

```cpp
DEKI_EXPORT
DEKI_UNIT(Distance)
float width;        // metres, like every other Distance
```
