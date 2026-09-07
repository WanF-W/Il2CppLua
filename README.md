<div align="center">

# 🧩 Il2CppLua

在 Unity IL2CPP 游戏进程中使用 Lua 检查类型、操作对象、调用方法与 Hook 逻辑

**v4.0.0** · Windows x64 · Lua 5.4.8 · MIT

</div>

## 📑 目录

- [项目定位](#project-positioning)
- [主要能力](#capabilities)
- [内部架构与目录](#architecture)
- [快速开始](#quick-start)
- [API 模型](#api-model)
- [API 参考](#api-reference)
  - [`lua`](#api-lua)
  - [`il2cpp`](#api-il2cpp)
  - [`Assembly`](#api-assembly)
  - [`Class`](#api-class)
  - [`Instance`](#api-instance)
  - [`Method`](#api-method)
  - [`Field`](#api-field)
- [Lua 与 IL2CPP 类型映射](#type-mapping)
- [Hook 与线程模型](#hook-threading)
- [通信与版本校验](#protocol-version)
- [Lune 命令行](#lune-cli)
- [构建](#build)
- [验证与测试脚本](#verification)
- [已知限制](#limitations)
- [License](#license)

## 🎯 项目定位

Il2CppLua 是注入 Unity IL2CPP 游戏进程的原生运行时桥接 DLL。它从
`GameAssembly.dll` 动态解析 IL2CPP API，将程序集、类、对象、方法和字段映射为
Lua userdata，不依赖游戏 SDK 或预生成的 dump 头文件。

配套控制台 Lune 负责定位进程、注入 DLL、执行 Lua 文件和提供交互式 REPL。
使用 `-i` 选择 IL2CPP 后端；该后端必须支持 `Il2CppLua/4.0.0` 握手。
Lune 自身的产品版本独立维护，无需与 DLL 版本相同。

Il2CppLua 适合正常使用 IL2CPP 运行时、并保留必要 `il2cpp_*` 导出函数的 Windows x64
游戏。它不是 Mono 调试器，也不负责解析未加载的 metadata 文件。

<a id="capabilities"></a>

## ✨ 主要能力

- 在全部程序集或指定程序集内查找类型
- 枚举程序集、类、方法和字段
- 创建托管对象与一维托管数组
- 读写实例字段和静态字段
- 调用实例方法、静态方法及精确重载
- 遍历和修改 IL2CPP 数组与 `List<T>`
- 查找存活的 `UnityEngine.Object` 实例
- Hook 原生方法，并在回调中选择调用原实现
- 将 Lua 回调投递到 Unity 主线程
- 对裸对象地址进行基础可读性保护后包装

<a id="architecture"></a>

## 🧱 内部架构与目录

项目把“运行时能力”“Lua 生命周期”“Lua API”“通信”和“Unity 业务适配”分开，依赖方向保持
从外到内：通信/入口 → Lua 引擎与绑定 → IL2CPP Resolver；Hook 和调度器只通过明确的
接口协作，不把 Unity 类名塞进通用运行时层。

```text
Lune
  │ 命名管道帧
  ▼
PipeChannel ──> dll_main 工作线程 ──> LuaEngine ──> lua_binding_* ──> Il2CppResolver
                                      │                 │
                                      │                 ├─ lua_value / lua_container
                                      │                 └─ UnityObjectQuery（Unity 专用适配）
                                      └─ Il2CppHook ──> hook_stub.asm
                                         │
                                         └─ Il2CppScheduler（tick 选择与任务队列）
```

| 目录/文件 | 单一职责 |
| --- | --- |
| `src/dll_main.cpp` | DLL 工作线程、初始化顺序、消息循环和逆序关闭 |
| `src/pipe_channel.*`、`src/protocol.h` | 命名管道、帧边界、并发发送与版本协议 |
| `src/il2cpp_resolver.*` | 动态解析 `GameAssembly.dll` 导出和通用反射/运行时调用 |
| `src/lua_engine.*` | Lua VM 生命周期、互斥访问、代码/文件执行和输出回调 |
| `src/lua_binding_*.cpp` | 面向 Lua 的 Assembly/Class/Instance/Method/Field API |
| `src/lua_method_call.cpp` | 方法重载选择、参数存储与运行时调用 |
| `src/lua_value.cpp`、`src/lua_container.cpp` | 统一类型编组、返回值转换、Array/List/table 操作 |
| `src/il2cpp_hook.cpp`、`src/hook_stub.asm` | Hook 注册、原生 ABI 调用、回调分发和原函数回退 |
| `src/il2cpp_scheduler.*` | tick 选择、目标线程识别和 Lua 任务队列 |
| `src/unity_object_query.*` | `UnityEngine.Object` 查询；不污染通用 Resolver |
| `lua_src/`、`minhook_src/` | 固定版本的第三方源码，不属于业务层，不应随意改动 |

### 生命周期与并发边界

初始化顺序是 `PipeChannel → Il2CppResolver → LuaEngine → Lua API`。关闭时：
先禁用 Hook 并等待所有 detour（包括仍在执行的原方法回退）退出，再释放 Lua、IL2CPP
线程和管道。Lua VM 的所有访问由可重入互斥锁串行化；Hook 注册表、调度队列和管道写入
各自使用独立锁，Hook 内部固定先取得 Lua 锁，再取得注册表锁。

工作线程只在初始化、执行命令和清理阶段附着到 IL2CPP；等待 Lune 命令时会主动脱离，
避免游戏退出时运行时等待控制线程。进程终止路径不等待 DLL 内部 I/O，资源由操作系统回收。

Resolver 只提供稳定的 IL2CPP 原语和反射信息，不直接知道 `UnityEngine`；Unity 专用查找
放在 `unity_object_query.*`。数组目前只实现一维零基 `SZARRAY`，引用数组写入通过运行时
写屏障完成。

Instance userdata 持有强 GCHandle，Lua GC 时释放，避免跨命令保存对象后失效。句柄固定对象地址，使原生地址接口保持稳定；这不阻止 `UnityEngine.Object.Destroy` 销毁原生实体。类型名由 Resolver 复制、缓存并释放运行时分配。初始化会检查必需的运行时导出，缺失时明确失败；异常文本使用的 object_to_string 为可选导出。

参数评分、普通调用、字段、数组及 Hook 共用类型转换规则：整数拒绝小数；字符串保留嵌入 NUL；接口兼容性使用运行时判断。`get_method` 和自动重载选择会查找父类，构造函数除外。引用类型 `new()` 必须找到匹配构造函数；仅值类型允许无显式构造函数的默认初始化。

调度器一批任务执行期间不会重入排空队列；任务中新提交的回调留待下一次外层 tick。`set_tick` 安装新入口失败时保留旧入口。长日志按不超过 64 KiB 的帧切分，仍受队列总量限制。

版本变化与升级说明见 [4.0.0 重构说明](RELEASE_NOTES_4.0.0.md)。

<a id="quick-start"></a>

## 🚀 快速开始

将 `Il2CppLua.dll` 4.0.0 与支持该版本的 `Lune.exe` 放在同一目录，启动游戏后执行：

```powershell
Lune.exe -i -n Game.exe
```

也可以按 PID 注入，或指定 DLL 与启动脚本：

```powershell
Lune.exe -i -p 1234 -d C:\Tools\Il2CppLua.dll -l C:\Scripts\startup.lua
```

进入 `ilune >>` 后即可输入 Lua：

```lua
local game = il2cpp.get_assembly("Assembly-CSharp")
local playerClass = game:get_class("Game", "Player")
il2cpp.schedule(function()
    local players = playerClass:find_unity_objects()
    if players and players[1] then
        local player = players[1]
        player:write_field("health", 999)
        print(player:read_field("health"))
        player:call("RefreshStatus")
    end
end)
```

示例中的程序集名、命名空间、类名、字段名和方法名必须替换为目标游戏的真实元数据。
执行 Unity 对象操作前，应确认调度 tick 属于主线程；默认入口不适用时，先使用
[`il2cpp.set_tick`](#il2cpp-set-tick) 指定游戏的主线程方法。

<a id="api-model"></a>

## 🧭 API 模型

| 层级 | 用途 |
| --- | --- |
| `lua` | 只处理普通 Lua table |
| `il2cpp` | 运行时状态、全局查找、Hook 清理和主线程调度 |
| `Assembly` | 程序集范围内的类型查找与枚举 |
| `Class` | 类型信息、创建对象、静态成员和 Unity 对象查找 |
| `Instance` | 实例成员，以及数组和 `List<T>` 容器操作 |
| `Method` | 精确方法信息、调用和 Hook |
| `Field` | 精确字段信息与读写 |

Lua table 使用 `lua.each` / `lua.dump`。IL2CPP 数组和 `List<T>` 使用
`instance:each`、`#instance` 和数字下标。Class 与普通 Instance 各自提供反射 dump。

<a id="api-reference"></a>

## 📚 API 参考

<a id="api-lua"></a>

### 🧰 `lua`：Lua table 工具

| API | 作用 |
| --- | --- |
| [`lua.each`](#lua-each) | 遍历普通 Lua table |
| [`lua.dump`](#lua-dump) | 输出 table 的第一层键值 |
| [`lua.hex`](#lua-hex) | 将整数或 lightuserdata 格式化为十六进制字符串 |

<a id="lua-each"></a>

#### `lua.each(table, callback)`

遍历普通 Lua table。回调参数为 `value, key`；数组表的 key 从 1 开始。

```lua
lua.each({ "a", "b", "c" }, function(value, key)
    print(key, value)
end)

lua.each({ hp = 100, mp = 50 }, function(value, key)
    print(key, value)
end)
```

<a id="lua-dump"></a>

#### `lua.dump(table)`

输出普通 Lua table 的第一层键值，不递归展开嵌套对象。

```lua
lua.dump({ name = "Player", level = 20 })
```

<a id="lua-hex"></a>

#### `lua.hex(value)`

将 Lua integer 或 lightuserdata 格式化为以 `0x` 开头的大写十六进制字符串。它只改变
显示形式，不改变地址和偏移 API 的 integer 返回类型。

```lua
print(lua.hex(24))                    -- 0x18
print(lua.hex(field:get_offset()))    -- 例如 0x2C
print(lua.hex(method:get_address()))  -- 例如 0x7FF93CC98430
print(lua.hex(instance:get_address()))
print(lua.hex(class:get_address()))
```

返回结果是字符串，适合输出和日志；地址计算仍直接使用原始 integer：

```lua
local address = instance:get_address() + field:get_offset()
print(lua.hex(address))
```

<a id="api-il2cpp"></a>

### ⚙️ `il2cpp`：运行时入口

| API | 作用 |
| --- | --- |
| [`il2cpp.get_status`](#il2cpp-get-status) | 获取运行时、程序集、调度器和模块状态 |
| [`il2cpp.is_initialized`](#il2cpp-is-initialized) | 判断 IL2CPP 是否初始化完成 |
| [`il2cpp.get_assemblies`](#il2cpp-get-assemblies) | 枚举全部程序集 |
| [`il2cpp.get_assembly`](#il2cpp-get-assembly) | 按名称查找程序集 |
| [`il2cpp.get_class`](#il2cpp-get-class) | 跨程序集查找类型 |
| [`il2cpp.wrap`](#il2cpp-wrap) | 将裸地址包装为 Instance |
| [`il2cpp.unhook_all`](#il2cpp-unhook-all) | 禁用全部用户方法 Hook |
| [`il2cpp.schedule`](#il2cpp-schedule) | 向 Unity 主线程投递任务 |
| [`il2cpp.set_tick`](#il2cpp-set-tick) | 设置主线程调度 tick |
| [`il2cpp.get_tick`](#il2cpp-get-tick) | 获取当前 tick 方法签名 |
| [`il2cpp.is_tick_ready`](#il2cpp-is-tick-ready) | 判断 tick Hook 是否就绪 |

<a id="il2cpp-get-status"></a>

#### `il2cpp.get_status()`

返回多行状态文本，包括初始化状态、导出函数解析情况、程序集数量、镜像数量、主线程
调度状态和 `GameAssembly.dll` 模块基址。

```lua
print(il2cpp.get_status())
```

输出形式如下：

```text
Initialized: true
Exports: All 61 functions resolved
Assemblies: 96
Images: 96
Main thread: ready
GameAssembly: 0x00007FFB12340000
```

<a id="il2cpp-is-initialized"></a>

#### `il2cpp.is_initialized()`

返回运行时是否完成初始化。

```lua
assert(il2cpp.is_initialized(), "IL2CPP runtime is not ready")
```

<a id="il2cpp-get-assemblies"></a>

#### `il2cpp.get_assemblies()`

返回包含全部 `Assembly` userdata 的 Lua 数组。

```lua
lua.each(il2cpp.get_assemblies(), function(assembly)
    print(assembly:get_name())
end)
```

<a id="il2cpp-get-assembly"></a>

#### `il2cpp.get_assembly(name)`

按名称返回 `Assembly`，找不到时返回 `nil`。名称比较忽略大小写，也可省略 `.dll`。

```lua
local game = il2cpp.get_assembly("Assembly-CSharp")
local core = il2cpp.get_assembly("mscorlib.dll")
print(game, core)
```

<a id="il2cpp-get-class"></a>

#### `il2cpp.get_class(namespace, name)`

遍历全部已加载程序集，返回第一个匹配的 `Class`。若多个程序集存在同名类型，应使用
`assembly:get_class` 消除歧义。

```lua
local player = il2cpp.get_class("Game", "Player")
local globalType = il2cpp.get_class("", "GlobalManager")
```

<a id="il2cpp-wrap"></a>

#### `il2cpp.wrap(address)`

将整数地址或 lightuserdata 包装为 `Instance`。函数会检查对象地址和对象头中的
`Il2CppClass*` 是否可读，但无法证明任意地址一定是长期有效的托管对象。

```lua
local obj = il2cpp.wrap(0x000001ABCDEF1230)
if obj then print(obj:get_class():get_full_name()) end

local invalid, err = il2cpp.wrap(0)
if not invalid then print(err) end
```

成功时只返回 Instance；失败时返回 `nil, error`。

<a id="il2cpp-unhook-all"></a>

#### `il2cpp.unhook_all()`

禁用全部用户方法 Hook。内部主线程 tick Hook 不属于用户 Hook。

```lua
il2cpp.unhook_all()
```

<a id="il2cpp-schedule"></a>

#### `il2cpp.schedule(callback)`

将无参 Lua 函数加入 Unity 主线程队列。函数在后续 tick 执行，不同步返回回调结果。

```lua
il2cpp.schedule(function()
    print("running on Unity main thread")
end)
```

<a id="il2cpp-set-tick"></a>

#### `il2cpp.set_tick(method)`

使用具有原生地址的精确 `Method` 作为调度 tick。成功返回 `true`；失败返回
`false, error`。应选择稳定、频繁且运行在 Unity 主线程的方法。

静态方法和实例方法都可以作为 tick。重复设置同一个 Method、切回曾经使用过的 Method，
或选择已经安装用户 Hook 的 Method 时，会复用现有原生 Hook；用户回调与调度 tick 可共存。

```lua
local time = il2cpp.get_class("UnityEngine", "Time")
local ok, err = il2cpp.set_tick(time:get_method("get_deltaTime"))
if not ok then print(err) end

local loop = il2cpp.get_class("Game", "GameLoop")
assert(il2cpp.set_tick(loop:get_method("Update", "System.Single")))
```

<a id="il2cpp-get-tick"></a>

#### `il2cpp.get_tick()`

返回当前 tick 的完整方法签名；尚未安装时返回 `nil`。

```lua
print(il2cpp.get_tick() or "tick is not installed")
```

<a id="il2cpp-is-tick-ready"></a>

#### `il2cpp.is_tick_ready()`

返回内部 tick Hook 是否已经安装。

```lua
if il2cpp.is_tick_ready() then
    il2cpp.schedule(function() print("ready") end)
end
```

初始化时会依次尝试 `UnityEngine.Time.get_deltaTime`、`get_frameCount` 和
`UnityEngine.Object.get_name`。默认入口不适用时，再使用 `set_tick` 替换。

<a id="api-assembly"></a>

### 📦 `Assembly`：程序集

| API | 作用 |
| --- | --- |
| [`assembly:get_name`](#assembly-get-name) | 获取程序集镜像名称 |
| [`assembly:get_class`](#assembly-get-class) | 在当前程序集内查找类型 |
| [`assembly:get_classes`](#assembly-get-classes) | 枚举当前程序集声明的类型 |

<a id="assembly-get-name"></a>

#### `assembly:get_name()`

返回程序集镜像名称。

```lua
local assembly = il2cpp.get_assembly("Assembly-CSharp")
print(assembly:get_name())
```

<a id="assembly-get-class"></a>

#### `assembly:get_class(namespace, name)`

只在当前程序集内查找类型，不回退到全程序集搜索。

```lua
local game = il2cpp.get_assembly("Assembly-CSharp")
local player = game:get_class("Game", "Player")
local globalType = game:get_class("", "GlobalManager")
```

<a id="assembly-get-classes"></a>

#### `assembly:get_classes()`

返回程序集声明的全部 `Class`。类枚举导出属于初始化必需项。

```lua
lua.each(assembly:get_classes(), function(cls)
    print(cls:get_full_name())
end)
```

`print(assembly)` 会输出程序集名称与原生地址：

```lua
print(assembly)
```

<a id="api-class"></a>

### 🧬 `Class`：类型

| API | 作用 |
| --- | --- |
| [`get_name` 等类型信息](#class-information) | 获取名称、继承关系、类型特征、大小和地址 |
| [`class:get_method`](#class-get-method) | 按名称或参数签名查找方法 |
| [`class:get_methods`](#class-get-methods) | 枚举当前类型声明的方法 |
| [`class:get_field`](#class-get-field) | 按名称查找字段 |
| [`class:get_fields`](#class-get-fields) | 枚举当前类型声明的字段 |
| [`class:new`](#class-new) | 分配对象并调用构造函数 |
| [`class:alloc`](#class-alloc) | 只分配对象，不调用构造函数 |
| [`class:new_array`](#class-new-array) | 创建一维托管数组 |
| [`class:static_call`](#class-static-call) | 调用静态方法 |
| [`class:read_static_field` / `write_static_field`](#class-static-field) | 读写静态字段 |
| [`class:find_unity_objects`](#class-find-unity-objects) | 查找存活的 Unity 对象 |
| [`class:dump`](#class-dump) | 输出类型的字段和方法概览 |

<a id="class-information"></a>

#### 类型信息

```lua
local cls = il2cpp.get_assembly("Assembly-CSharp"):get_class("Game", "Player")

print(cls:get_name())
print(cls:get_namespace())
print(cls:get_full_name())
print(cls:get_assembly())
print(cls:get_parent())
print(cls:is_value_type())
print(cls:is_enum())
print(cls:get_instance_size())
print(string.format("0x%X", cls:get_address()))
print(cls)
```

这组示例覆盖 `get_name`、`get_namespace`、`get_full_name`、`get_assembly`、
`get_parent`、`is_value_type`、`is_enum`、`get_instance_size` 和 `get_address`。

<a id="class-get-method"></a>

#### `class:get_method(name [, parameterType...])`

不传参数类型时返回第一个同名方法；传入类型时按完整参数签名选择重载。

```lua
local update = cls:get_method("Update")
local byId = cls:get_method("FindItem", "int")
local byName = cls:get_method("FindItem", "System.String")
local apply = cls:get_method("Apply", "Game.Player", "float")
```

支持 `bool`、`byte`、`sbyte`、`char`、`short`、`ushort`、`int`、`uint`、`long`、
`ulong`、`float`、`double`、`string`、`object`、`void` 简写，也支持完整类型名和
`int[]` 等数组形式。找不到精确重载时返回 `nil`。

<a id="class-get-methods"></a>

#### `class:get_methods()`

返回当前类的方法数组。

```lua
lua.each(cls:get_methods(), function(method)
    print(method:get_signature())
end)
```

<a id="class-get-field"></a>

#### `class:get_field(name)`

返回精确 `Field`，找不到时返回 `nil`。

```lua
local healthField = cls:get_field("health")
if healthField then print(healthField:get_signature()) end
```

<a id="class-get-fields"></a>

#### `class:get_fields()`

返回当前类的字段数组。

```lua
lua.each(cls:get_fields(), function(field)
    print(field:get_signature())
end)
```

<a id="class-new"></a>

#### `class:new(...)`

分配对象、初始化类并调用匹配的构造函数。构造函数按 Lua 参数数量与类型选择。

```lua
local empty = cls:new()
local named = cls:new("Wukong")
local positioned = cls:new(10, 20)
```

找不到匹配构造函数时会报错。仅值类型允许无参数、无显式 `.ctor` 的默认初始化；
引用类型若需要有意跳过构造函数，应使用 `alloc()`。

<a id="class-alloc"></a>

#### `class:alloc()`

只分配对象并初始化类，不调用实例构造函数。

```lua
local rawObject = cls:alloc()
rawObject:write_field("health", 100)
```

<a id="class-new-array"></a>

#### `class:new_array(length)`

创建以当前 Class 为元素类型的一维托管数组。Lua 访问下标从 1 开始。

```lua
local intClass = il2cpp.get_class("System", "Int32")
local numbers = intClass:new_array(3)
numbers[1], numbers[2], numbers[3] = 10, 20, 30
print(#numbers)

local playerArray = cls:new_array(2)
playerArray[1] = cls:new()
playerArray[2] = nil
```

<a id="class-static-call"></a>

#### `class:static_call(name, ...)`

按 Lua 实参自动选择并调用静态方法重载。

```lua
local manager = il2cpp.get_class("Game", "PlayerManager")
local current = manager:static_call("GetCurrent")
manager:static_call("SetDifficulty", 2)
```

<a id="class-static-field"></a>

#### `class:read_static_field(name)` / `class:write_static_field(name, value)`

按名称读写静态字段。实例字段传给这些接口会明确报错。

```lua
print(manager:read_static_field("Instance"))
manager:write_static_field("DebugEnabled", true)
print(manager:read_static_field("DebugEnabled"))
```

`const` 字段不可写；`readonly` 字段目前按运行时字段写入能力处理。

<a id="class-find-unity-objects"></a>

#### `class:find_unity_objects()`

通过 `UnityEngine.Object.FindObjectsOfType` 返回当前类型的活跃 Unity 对象数组。
它只适用于继承 `UnityEngine.Object` 的类型，不是通用托管堆遍历。

```lua
local enemyClass = il2cpp.get_class("Game", "EnemyController")
local enemies = enemyClass:find_unity_objects()
if enemies then
    lua.each(enemies, function(enemy, index)
        print(index, enemy:get_address())
    end)
end
```

<a id="class-dump"></a>

#### `class:dump()`

输出当前类的程序集、类型种类、父类、地址、实例大小，以及当前类声明的字段和方法。

```lua
cls:dump()
```

<a id="api-instance"></a>

### 🎮 `Instance`：托管对象与容器

| API / 语法 | 作用 |
| --- | --- |
| [`instance:call`](#instance-call) | 调用实例方法 |
| [`instance:read_field` / `write_field`](#instance-field) | 读写实例字段 |
| [`instance:get_class` / `get_address`](#instance-information) | 获取实际类型和对象地址 |
| [`instance:dump`](#instance-dump) | 输出对象字段或容器元素 |
| [`#obj` / `obj[index]`](#instance-container-syntax) | 获取和修改数组或 List 元素 |
| [`instance:each`](#instance-each) | 遍历数组或 List |

<a id="instance-call"></a>

#### `instance:call(name, ...)`

按 Lua 实参自动选择并调用实例方法重载。

```lua
local player = cls:new()
player:call("SetLevel", 20)
print(player:call("GetLevel"))
player:call("Teleport", 10.0, 20.0, 30.0)
```

返回类型为 `void` 的方法不产生 Lua 返回值，因此在 Lune 中不会回显 `nil`。返回类型
是引用类型但实际结果为 null 的方法仍会返回一个 Lua `nil`。

<a id="instance-field"></a>

#### `instance:read_field(name)` / `instance:write_field(name, value)`

按名称读写实例字段。静态字段必须使用 Class 接口。

```lua
print(player:read_field("health"))
player:write_field("health", 999)

local target = player:read_field("target")
player:write_field("target", target)
player:write_field("target", nil)
```

<a id="instance-information"></a>

#### `instance:get_class()` / `instance:get_address()`

返回对象的 Class 和原生对象地址。

```lua
print(player:get_class():get_full_name())
print(string.format("0x%X", player:get_address()))
print(player)
```

<a id="instance-dump"></a>

#### `instance:dump([includeParents])`

普通对象默认只输出实际类型自身声明的实例字段和值；传 `true` 时按从基类到派生类
的顺序同时输出父类字段。

```lua
player:dump()
player:dump(true)
```

静态字段不属于具体对象，因此不会出现在 Instance dump 中。

当 Instance 是数组或 `List<T>` 时，`dump()` 不读取容器的内部实现字段，而是按
`each` 相同的顺序直接输出逻辑元素：

```lua
local items = player:read_field("items")
items:dump()
```

```text
List: System.Collections.Generic.List<HeroData>
count: 3
[1] = Instance: HeroData @ 0x000001F000001000
[2] = Instance: HeroData @ 0x000001F000002000
[3] = Instance: HeroData @ 0x000001F000003000
```

<a id="instance-container-syntax"></a>

#### 数组与 `List<T>`：`#obj`、`obj[index]`、`obj[index] = value`

数组和 `List<T>` 都使用 Lua 1 基索引，工具会自动转换为 C# 0 基索引。

```lua
local inventory = player:read_field("items")
print(#inventory)
print(inventory[1])
inventory[1] = inventory[2]
```

数组支持基本类型、引用类型和值类型元素。`List<T>` 下标访问分别调用运行时的
`get_Item` 和 `set_Item`。

<a id="instance-each"></a>

#### `instance:each(callback)`

只对数组和 `List<T>` 有效。回调参数为 `value, index`。

```lua
inventory:each(function(item, index)
    print(index, item)
end)
```

普通对象调用 `each`、长度运算或数字下标会抛出 Lua 错误。

这些语法分别由 Instance 的 `__len`、`__index` 和 `__newindex` 元方法实现；
`print(instance)`、`print(class)` 等可读文本由各 userdata 的 `__tostring` 实现，
它们是 Lua 语法支持，不需要也不应由用户直接调用。

<a id="api-method"></a>

### 🔧 `Method`：精确方法

| API | 作用 |
| --- | --- |
| [`get_name` 等方法信息](#method-information) | 获取名称、声明类型、完整签名和原生地址 |
| [`method:call`](#method-call) | 精确调用实例或静态方法 |
| [`method:hook`](#method-hook) | 安装 Lua 方法 Hook |
| [`method:is_hooked` / `unhook`](#method-hook-state) | 查询或移除当前方法 Hook |

<a id="method-information"></a>

#### 方法信息

```lua
local method = cls:get_method("TakeDamage", "float")
print(method:get_name())
print(method:get_class():get_full_name())
print(method:get_signature())
print(string.format("0x%X", method:get_address()))
print(method)
```

这组示例覆盖 `get_name`、`get_class`、`get_signature` 和 `get_address`。
`get_signature` 包含访问级别、修饰符、返回类型、声明类、参数类型及参数名。

<a id="method-call"></a>

#### `method:call(instance, ...)`

实例方法的第一个参数必须是兼容的 Instance；静态方法不传 Instance。

```lua
local getLevel = cls:get_method("GetLevel")
print(getLevel:call(player))

local setLevel = cls:get_method("SetLevel", "int")
setLevel:call(player, 30)

local manager = il2cpp.get_class("Game", "PlayerManager")
local current = manager:get_method("GetCurrent"):call()
```

`void` 方法返回 0 个 Lua 值；非 void 方法返回 1 个值，包括用于表示 null 引用的 `nil`。
普通调用和 Hook 均明确拒绝 `ref/out` 参数及 `ref` 返回值。普通调用可使用已有的闭合泛型方法，不能调用开放泛型定义。

<a id="method-hook"></a>

#### `method:hook(callback)`

实例方法回调签名为 `function(this, original, ...)`：

```lua
local damage = cls:get_method("TakeDamage", "float")
damage:hook(function(this, original, amount)
    print("TakeDamage", this, amount)
    return original(amount * 0.5)
end)
```

静态方法的第一个回调参数是声明 Class：

```lua
local calculate = cls:get_method("CalculateScore", "int")
calculate:hook(function(declaringClass, original, value)
    print(declaringClass:get_full_name(), value)
    return original(value) * 2
end)
```

`original()` 不传参数时透传捕获到的参数；传入参数时使用替换参数调用原实现。不调用
`original` 时，回调返回值直接成为原生方法返回值：

```lua
damage:hook(function(this, original, amount)
    return 0
end)
```

<a id="method-hook-state"></a>

#### `method:is_hooked()` / `method:unhook()`

```lua
if damage:is_hooked() then
    damage:unhook()
end
```

<a id="api-field"></a>

### 🏷️ `Field`：精确字段

| API | 作用 |
| --- | --- |
| [`get_name` 等字段信息](#field-information) | 获取名称、声明类型、完整签名和实例偏移 |
| [`field:read` / `field:write`](#field-read-write) | 精确读写实例或静态字段 |

<a id="field-information"></a>

#### 字段信息

```lua
local field = cls:get_field("health")
print(field:get_name())
print(field:get_class():get_full_name())
print(field:get_signature())
print(field:get_offset())
print(field)
```

这组示例覆盖 `get_name`、`get_class`、`get_signature` 和 `get_offset`。静态字段
没有实例内偏移，`get_offset()` 返回 `nil`。

<a id="field-read-write"></a>

#### `field:read(...)` / `field:write(...)`

实例字段需要传入兼容的 Instance：

```lua
local health = cls:get_field("health")
print(health:read(player))
health:write(player, 500)
```

静态字段不传 Instance：

```lua
local manager = il2cpp.get_class("Game", "PlayerManager")
local instanceField = manager:get_field("Instance")
print(instanceField:read())
instanceField:write(player)
```

Field 接口适合需要避免同名字段歧义的场景。`const` 字段不可写。

<a id="type-mapping"></a>

## 🔄 Lua 与 IL2CPP 类型映射

| IL2CPP 类型 | Lua 表示 |
| --- | --- |
| `bool` | boolean |
| 有符号/无符号整数、char | integer |
| `float` / `double` | number |
| `System.String` | string 或 `nil` |
| class / object / array | Instance 或 `nil` |
| `IntPtr` / `UIntPtr` | integer |
| 原生指针 / 函数指针 | lightuserdata |
| struct | 装箱后的 Instance |
| enum | 底层整数对应的 Lua integer |

Lua integer 是有符号 64 位；读取大于 `INT64_MAX` 的 `ulong` 时会按原始位模式表现为
负数。结构体和枚举的临时存储大小来自 IL2CPP 运行时，按实际类型分配存储。
引用参数可以传兼容 Instance 或 `nil`；一维引用数组的写入会经过 IL2CPP 写屏障。

<a id="hook-threading"></a>

## 🪝 Hook 与线程模型

- Lua VM 由可重入互斥锁串行访问。
- Hook 回调可能来自任意游戏线程。
- 进入 Lua 前，Hook 分发器会尝试附加当前 IL2CPP 线程。
- `il2cpp.schedule` 的任务只在 tick 首次实际触发的线程中执行；默认 tick 只是候选入口，
  不能保证每个游戏都运行在 Unity 主线程，必要时应显式调用 `set_tick`。
- Hook 回调内可以再次调用方法，也可以调用 `original()`。
- `original()` 直接调用 MinHook trampoline，保留原生 `this` 和 `MethodInfo`，无参时沿用原参数，有参时复用统一转换规则；不临时禁用全局 Hook。
- 值类型 Hook 的 `this` 是供 Lua 查看或操作的装箱快照；对快照的修改不会写回原生 `this`。原方法对原生 `this` 的修改正常保留。
- `original()` 抛出的原生或托管异常转换为 Lua 错误，不重试；未完成调用没有可复用结果时使用默认返回值并记录错误。
- 回调报错或返回值不兼容时，如果尚未调用 `original`，调用原方法一次；如果已经调用过，则复用已完成调用的返回值，不重复副作用。
- Hook 最多接受 64 个声明参数；不支持 `ref/out`、`ref` 返回或泛型方法 Hook。
- 卸载时先禁用 Hook，并等待完整 detour（包括回退路径中仍在执行的原方法）结束后再释放。
- 日志通过有界异步队列发送，Hook 和游戏线程不会因管道写入长期阻塞；队列满时会丢弃新日志。

不要在高频 Hook 中执行大量打印、文件 IO 或长时间 Lua 计算。只能在主线程访问的
Unity 对象，应通过 `il2cpp.schedule` 操作。

<a id="protocol-version"></a>

## 📡 通信与版本校验

Lune 创建命名管道并注入 DLL，DLL 连接后发送：

```text
MSG_HELLO: Il2CppLua/4.0.0
```

Lune 会将该字符串与 `src/backend_profile.h` 中 `IL2CPP_PROFILE.protocolVersion` 精确比较。版本不同会显示 expected 与
received，并在等待 READY 或进入 REPL 前终止连接。

`MSG_ERROR` 的负载为 `[1 字节错误类别][4 字节可选行号 little-endian][UTF-8 错误文本]`；`MSG_FILE` 执行中的 Lua 错误可填写行号，普通命令（包括 Lune 的 `-l`）和其他类别使用 `-1`。
CLI 只显示 `Lua Error`、`Il2Cpp Error`、`CSharp Error` 或 `Lune Error`，不再把 Lua
内部的 source name（例如 `[string "<string>"]`）暴露给用户。

每个协议帧的负载上限为 1 MiB。Lune 的 `-l` 将 `dofile(绝对路径)` 作为普通命令发送，
使用 Lua 标准文件加载行为。DLL 另保留 `MSG_FILE` 文件入口，按 UTF-8 路径读取完整文件，
上限为 4 MiB；此限制不适用于 `dofile` / `loadfile`。

长日志按最多 64 KiB 一帧切分，异步队列总量上限为 4 MiB；队列满时丢弃新日志。

DLL 的产品版本、协议版本和 Windows 文件版本来自 `src/version.h`。发布时同步更新
Lune 的 `src/backend_profile.h` 中 IL2CPP 后端的显示版本与握手字符串；
Lune 的 `src/version.h` 只表示控制端自身版本。

<a id="ilune-cli"></a>
<a id="lune-cli"></a>

## 💻 Lune 命令行

```text
Lune.exe -i -n <进程名> [-d <DLL路径>] [-l <Lua脚本>]
Lune.exe -i -p <PID>    [-d <DLL路径>] [-l <Lua脚本>]
```

| 参数 | 说明 | 示例 |
| --- | --- | --- |
| `-i` | 选择 IL2CPP 后端（必填） | `Lune.exe -i -n Game.exe` |
| `-n`, `--name` | 按进程名注入 | `Lune.exe -i -n Game.exe` |
| `-p`, `--pid` | 按 PID 注入 | `Lune.exe -i -p 1234` |
| `-d`, `--dll` | 指定 Il2CppLua.dll | `Lune.exe -i -n Game.exe -d D:\Tools\Il2CppLua.dll` |
| `-l`, `--lua` | 握手完成后执行脚本 | `Lune.exe -i -n Game.exe -l D:\Scripts\start.lua` |

REPL 中输入表达式会自动作为 `return <表达式>` 执行，语句则原样执行。输入 `exit` 或
`quit` 会向 DLL 发送退出消息并关闭控制台。

<a id="build"></a>

## 🔨 构建

要求：Windows x64、Visual Studio（当前项目工具集 v145）、Windows SDK 10.0、MSVC v145 工具集，以及
MASM x64 构建支持。

打开 `Il2CppLua.slnx`，选择 `Release | x64` 生成 `Il2CppLua.dll`。Lune 也应使用
`Release | x64`。正式发布时配套提供支持 `Il2CppLua/4.0.0` 的 Lune 控制端。

源码中的 `src/hook_stub.asm` 必须由 MASM 编译；如果只使用命令行编译器检查 C++，仍需单独
编译该文件，不能把 Hook 跳板替换成普通 C++ 函数。`lua_src/` 和 `minhook_src/` 是项目
随附的第三方实现，业务修改应集中在 `src/`。

<a id="verification"></a>

## 验证与测试脚本

4.0.0 修复 `lua.each` 非数组键回调参数错误，以及无参、非 void 方法返回值丢失后，
维护者已确认功能测试通过。发布前的源码与文档整理不等同于重新执行测试。

[scripts/longyin_api_test.lua](scripts/longyin_api_test.lua) 是针对《龙胤立志传》的手动功能测试脚本，
依赖该游戏的类型和场景，不能直接用于其他游戏。使用前阅读脚本头部说明；其中包含对象创建、
字段写入、Hook 和异步调度，适合独立测试会话。它不验证 DLL 卸载并发、通信边界或原生资源泄漏。

<a id="limitations"></a>

## ⚠️ 已知限制

- 仅支持 Windows x64。
- 依赖目标运行时保留所需的 `il2cpp_*` 导出函数。
- `find_unity_objects` 只查找 `UnityEngine.Object`，不遍历完整托管堆。
- 类的方法与字段枚举当前各自最多保留 1024 项。
- `get_class` 全程序集搜索遇到同名类型时返回第一个结果。
- `get_method(name)` 遇到重载时返回第一个结果，应传参数类型进行精确选择。
- 泛型方法与泛型实例化方法不提供 Hook，避免共享代码和特殊 ABI 导致错误拦截；闭合泛型的普通调用仍可用。
- 只支持一维零基 `SZARRAY`；多维数组的 bounds 和索引未实现。
- 普通调用和 Hook 都拒绝 `ref/out` 参数及 `ref` 返回值。
- 引用数组使用 `il2cpp_gc_wbarrier_set_field`；含托管引用的 struct 数组元素暂不支持直接写入。
- 默认调度 tick 不能证明线程一定是 Unity 主线程；需要可靠线程语义时必须使用 `set_tick`。
- Hook 只接受标准 IL2CPP Windows x64 调用约定，最多 64 个声明参数。
- Hook 回调线程会附加到 IL2CPP，当前策略保持这些线程的运行时附加状态到进程结束，避免
  在未知回调生命周期中错误 detach。
- 任意裸地址即使当前可读，也可能在之后因 GC、对象销毁或内存复用而失效。

<a id="license"></a>

## 📄 License

MIT License，详见 [LICENSE](LICENSE)。
