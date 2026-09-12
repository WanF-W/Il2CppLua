# Il2CppLua 4.1.0 Release Notes

## 版本信息

- Il2CppLua 版本：`4.1.0`
- 协议版本：`Il2CppLua/4.1.0`
- 配套组件：Il2CppLua.dll `4.1.0` 与包含 IL2CPP 后端 `4.1.0` 握手配置的 Lune
- 整理依据：Il2CppLua 工作区相对 Git `HEAD` 的源码、工程文件和文档差异。
- 本次只进行源码和文档调整，未编译、未运行测试或启动游戏；下文描述的是已实现代码，不代表已经完成运行时验证。

<a name="version-and-pairing"></a>
## 1. 版本标识与配套要求

- [`src/version.h`](../src/version.h) 统一提供 DLL 文件版本、产品版本和协议版本字符串，Windows 资源脚本继续从这些宏读取文件属性。
- Lune 的 IL2CPP 后端握手标识同步为 `Il2CppLua/4.1.0`；DLL 与 Lune 配置不一致时，连接会在进入 READY 或 REPL 前终止。
- Lune 自身的产品版本独立维护，只有其 IL2CPP 后端的显示版本和 `protocolVersion` 需要与 DLL 配套。

<a name="lua-api-and-userdata"></a>
## 2. Lua 入口与 userdata 模型

- 全局 `lua` 入口提供 `each`、`dump`、`hex`；`hex` 支持 Lua integer 和 `lightuserdata` 的显示格式化。
- 全局 `il2cpp` 入口提供状态、程序集/类型查找、地址包装、Hook 清理和主线程调度接口：
  `get_status`、`is_initialized`、`get_assemblies`、`get_assembly`、`get_class`、`wrap`、`unhook_all`、`schedule`、`set_tick`、`get_tick`、`is_tick_ready`。
- Assembly、Class、Instance、Method 和 Field 使用独立 userdata 元表；数组与 `List<T>` 也由 Instance userdata 表示。
- Instance userdata 持有强对象句柄，Lua GC 时释放；句柄可以让托管对象地址在 Lua 引用存续期间保持稳定，但不能阻止 `UnityEngine.Object.Destroy` 销毁原生实体。
- 4.1.0 对输出、错误和会话状态的处理更明确：输出超限会显示截断标记，原始 Lua 错误对象可由 `pcall` 继续取得，原生故障会使当前会话进入隔离状态。

<a name="assembly-and-class"></a>
## 3. 程序集与类型元数据

- Assembly 支持名称读取、当前程序集内的 Class 查找和 Class 枚举；程序集名称比较忽略大小写，也可以省略 `.dll` 后缀。
- Class 支持名称、命名空间、完整名称、程序集、父类、值类型/枚举、实例大小和地址查询，并提供方法/字段查找与枚举。
- 方法与字段查询改为直接使用 IL2CPP 的元数据迭代器，不再因固定缓存数量截断查询结果。`get_methods()`、`get_fields()` 和重载查找不受 dump 展示预算影响。
- `Class:dump()` 与 `Instance:dump()` 仍是面向日志的有限展示：成员展示最多保留 1024 项，超过时明确显示截断；文本总长度超过预算时显示 `[dump truncated]`。
- 调用、字段、对象创建、数组元素和 Hook 使用前会检查声明类及相关编组类型是否闭合。桥接通过托管 `Type.ContainsGenericParameters` 识别开放类型，并缓存结果；反射信息不可用时保守拒绝操作。
- `System.Nullable<T>` 不按普通 struct 编组。Nullable 参数、返回值、字段、数组元素以及相关声明类操作会明确报错。
- Class 仍提供 `static_call`、静态字段读写、`alloc`、`new`、`new_array`、`find_unity_objects` 和 `dump`；静态成员查找与实例方法/字段查找遵循各自的继承规则。

<a name="method-and-field"></a>
## 4. 方法调用与字段读写

- 方法调用统一使用 Lua 参数评分、类型编组和返回值转换，覆盖整数、枚举、字符、浮点、布尔、字符串、对象引用、数组和装箱值类型。
- 窄整数、`char` 和枚举写入或作为参数时会检查目标范围，拒绝小数和越界值，不再静默截断；重载选择使用同一套范围规则。
- `ulong`、`UIntPtr` 及 64 位无符号枚举保留 64 位位模式；读取高位值时可能以有符号 Lua integer 的负数形式出现。
- `Method:call` 可精确调用实例或静态方法；`Instance:call` 和 `Class:static_call` 按 Lua 实参选择重载。普通调用允许已有的闭合泛型方法，但拒绝开放泛型方法。
- `Field:read` / `Field:write` 支持实例字段和静态字段；`Class:read_static_field` / `write_static_field` 是按名称操作静态字段的便捷接口。`const` 字段不可写。
- 普通调用和 Hook 都拒绝 `ref/out` 参数及 byref 返回值。缺少类型存储或运行时反射信息时，会返回 `Il2Cpp Error`，而不是继续进行不安全编组。
- 字符串按显式长度创建，可以保留嵌入的 NUL；Hook 中 `return original()` 得到的字符串会重新构造托管字符串，不保证保留原托管对象身份。

<a name="containers-and-object-creation"></a>
## 5. 对象创建、数组与 List<T>

- `Class:alloc()` 只分配对象，不调用构造函数；`Class:new(...)` 查找匹配构造函数并执行；值类型可以创建默认值。
- `Class:new_array(length)` 创建一维 `SZARRAY`。长度必须是非负整数且不超过 `uint32` 上限。
- 数组和 `System.Collections.Generic.List<T>` 支持 `#value`、`value[index]`、`value[index] = newValue` 和 `value:each(callback)`；Lua 索引从 1 开始，数组内部仍使用 IL2CPP 的 0 基索引。
- 数组元素与 List 元素使用统一类型检查。引用数组写入通过 IL2CPP 写屏障；含托管引用的 struct 数组元素写入以及多维数组仍不支持。
- `lua.each`、数组 `each` 和 List `each` 会直接传播回调抛出的原始 Lua 错误对象，不再截断或重新包装为固定长度的错误文本。
- 容器 `dump()` 输出逻辑元素而不是内部实现字段；普通对象 dump 可选择是否包含父类字段。dump 输出仍受文本和成员数量预算限制。

<a name="unity-query"></a>
## 6. Unity 对象查询

- `Class:find_unity_objects()` 只允许查询继承自 `UnityEngine.Object` 的类型，使用目标运行时可用的 Unity 查询入口返回活跃对象。
- 查询结果会转换为 Instance；它不是完整托管堆遍历，也不会返回不属于 Unity 对象层级的普通托管对象。
- 查询辅助不再额外使用固定的 512 项上限；查询结果仍受 Lua 内存、Unity 运行时和输出展示预算影响。
- Unity API 和对象查询应在 Unity 主线程执行。不能确认自动 tick 的线程语义时，应通过 `il2cpp.set_tick` 指定可靠入口。

<a name="hooks"></a>
## 7. IL2CPP Hook

- Hook 使用 Windows x64 原生跳板和 MinHook；`Method:hook` 支持实例方法、静态方法、值类型参数和支持的返回值。
- 实例方法回调格式为 `function(this, original, ...)`；静态方法回调的第一个参数是声明该方法的 Class。
- `original()` 不传参数时复用本次 Hook 捕获的参数；传入参数时按统一编组规则使用替代参数。回调也可以不调用 `original()`，直接返回替代结果。
- 回调报错或返回值不兼容时，若尚未调用 `original`，桥接会尝试调用原方法一次；若已经完成过原方法调用，则复用已完成结果，不重复原生副作用。
- 同一 Method 的用户 Hook 与调度 tick 可以共存；重复安装同一 Method 会替换回调。`Method:unhook()` 和 `il2cpp.unhook_all()` 只清理用户回调，不应破坏仍在使用的内部 tick。
- Hook 最多接受 64 个声明参数，要求标准 IL2CPP Windows x64 调用约定；开放泛型、闭合后的实例化泛型、`ref/out`、byref 返回和无法表示的 ABI 仍不支持。值类型 `this` 以装箱快照传给 Lua，修改快照不会写回原生 `this`。
- Hook 回调可能在任意游戏线程执行。高频回调中不要执行大量打印、文件 IO 或长时间 Lua 计算；只能在主线程访问的 Unity 对象应交给 `il2cpp.schedule`。

<a name="main-thread-scheduler"></a>
## 8. Unity 主线程调度

- `il2cpp.schedule(function)` 将无参 Lua 回调放入任务队列，由 tick Hook 在后续调用中执行，不同步返回回调结果。
- 默认会依次尝试 `UnityEngine.Time.get_deltaTime`、`UnityEngine.Time.get_frameCount` 和 `UnityEngine.Object.get_name` 作为 tick；也可以用 `il2cpp.set_tick(method)` 指定具有原生地址的稳定方法。
- 首次有效 tick 会固定任务执行线程；默认入口只是候选方法，不能保证一定是 Unity 主线程。`get_tick()` 返回当前 tick 签名，`is_tick_ready()` 返回安装状态。
- 待执行任务最多 1024 项。没有可用 tick 时仍允许有界排队；队列满或分配失败时 `schedule` 抛出 `Il2Cpp Error`，并释放新回调引用。
- 一批任务执行期间不会重入排空队列；回调中新加入的任务留到下一次外层 tick。tick 切换失败时保留原有可用 tick。

<a name="runtime-lifecycle"></a>
## 9. Runtime、对象生命周期与故障隔离

- 必需的 `il2cpp_*` 导出在初始化阶段检查；`il2cpp_object_to_string` 只用于增强诊断，缺失时不阻止运行。
- Resolver 的类型开放性检查带有递归保护，反射 getter 自身被 Hook 或反射信息不可用时不会假定类型安全可用。
- 普通退出会先禁用 Hook，等待完整 detour（包含仍在执行的原方法回退）结束，再释放 Lua、对象句柄、Resolver 和管道资源。
- 访问违例等原生故障会向最外层执行边界传播，并将当前 Lua 会话永久隔离。隔离后不再进入 Lua VM，不执行 Lua 栈恢复、registry 清理或 `lua_close`；后续 Hook 通过已发布的 trampoline 直接透传原方法。
- 故障隔离会保留 VM、对象句柄、Hook、trampoline 和 DLL 到目标进程结束，避免卸载仍可能被游戏线程跳转的代码；必须重启目标进程才能恢复。已进入的原方法不会自动重试，游戏状态和当前调用结果也不保证有效。
- 普通 Lua 错误以及 `runtime_invoke` 正常返回的托管异常不触发故障隔离，仍按普通命令或 Hook 错误路径处理。

<a name="protocol-errors-and-io"></a>
## 10. 协议、错误与输出

- 管道帧格式为 `[1 字节类型][4 字节小端长度][负载]`，单帧负载上限为 `1 MiB`。DLL 发送 `MSG_HELLO` 时使用 `Il2CppLua/4.1.0`。
- `MSG_ERROR` 负载包含错误类别、可选 Lua 行号和 UTF-8 错误文本；Lune 可区分 `Lua Error`、`Il2Cpp Error`、`CSharp Error` 和 `Lune Error`。
- CMD 按消息的完整字节长度执行，嵌入 NUL 的源码不会被提前截断；FILE 路径拒绝嵌入 NUL；空命令和空文件是成功的无操作。
- `print`、返回值回显以及每个命令、Hook、schedule 的输出捕获批次最多保留 `1 MiB`，超出时追加 `[output truncated]`。这只限制桥接层输出缓冲，不限制用户字符串本身的分配或 `__tostring` 执行。
- 日志按不超过 `64 KiB` 的 UTF-8 边界分帧；待发送队列最多 `1024` 帧、`4 MiB`。队列满、连接停止或分配失败时会拒绝或丢弃日志批次，不让游戏线程长期阻塞在管道写入上。
- `il2cpp.get_status()` 额外报告 `Rejected log batches`、`Discarded log frames` 和 `Dropped log bytes (lower bound)`，用于判断日志是否因队列或连接问题丢失。
- 写入超时会取消重叠 I/O，并在取消完成后释放相关资源；发送 READY、OK 或 ERROR 前会等待当时已接受的日志完成。连接窗口会兼容管道尚未创建或暂时忙的情况，控制帧发送失败时及时结束会话。

<a name="project-and-documentation"></a>
## 11. 工程与使用文档

- [`README.md`](../README.md) 重整为当前版本使用手册，集中描述安装、启动、Lua API、类型映射、线程/Hooks、Lune、构建和限制，不承载版本过程记录。
- 新增本 ReleaseNote，并在 `.release_local/` 增加本地维护规则和 `4.1.0` 发布大纲；发布大纲不进入 Git。
- Lune 的 [`src/backend_profile.h`](https://github.com/WanF-W/Lune/blob/master/src/backend_profile.h) 与 README 同步 IL2CPP `4.1.0` 握手配置。
- 本版本仍要求 Windows x64、Visual Studio v145 工具集、Windows SDK 10.0 和 MASM x64 支持；`lua_src/` 与 `minhook_src/` 为随附第三方源码。
