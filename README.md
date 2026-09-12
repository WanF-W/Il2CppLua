<div align="center">

# 🧩 Il2CppLua

在 Windows x64 Unity IL2CPP 游戏进程中使用 Lua 查找程序集、访问类型和对象、调用方法、读写字段以及 Hook 方法。

**v4.1.1** · Windows x64 · Lua 5.4.8 · MIT

</div>

## 📑 目录

- [✅ 使用条件](#usage-conditions)
- [🚀 启动与连接](#quick-start)
- [📚 Lua API](#api-reference)
  - [🧰 `lua`](#api-lua)
  - [⚙️ `il2cpp`](#api-il2cpp)
  - [📦 `Assembly`](#api-assembly)
  - [🧬 `Class`](#api-class)
  - [🎮 `Instance`](#api-instance)
  - [🔧 `Method`](#api-method)
  - [🏷️ `Field`](#api-field)
- [🔄 类型映射](#type-mapping)
- [🪝 线程与 Hook](#hook-threading)
- [📡 通信、错误与输出](#protocol-errors)
- [💻 Lune 命令行](#lune-cli)
- [🔨 构建](#build)
- [⚠️ 限制](#limitations)
- [📄 License](#license)

<a name="usage-conditions"></a>
## ✅ 使用条件

- Windows x64。
- 目标程序使用 Unity IL2CPP，并已加载 `GameAssembly.dll`。
- 目标运行时保留 Il2CppLua 所需的 `il2cpp_*` 导出函数。
- `Il2CppLua.dll` 与配套的 [Lune](../Lune) 使用同一套 IL2CPP 握手版本：`Il2CppLua/4.1.1`。

Il2CppLua 读取目标进程中已经加载的 IL2CPP 程序集和 metadata，不读取未加载的 metadata 文件，
也不适用于 Unity Mono 运行时。目标游戏、DLL 和 Lune 都必须是 Windows x64。

<a name="quick-start"></a>
## 🚀 启动与连接

将 `Lune.exe` 和 `Il2CppLua.dll` 放在同一目录。先启动游戏，再使用 Lune 的 IL2CPP 后端连接目标进程。

按进程名连接：

```bat
Lune.exe -i --name Game.exe
```

按 PID 连接：

```bat
Lune.exe -i --pid 1234
```

指定 DLL 和启动脚本：

```bat
Lune.exe -i --name Game.exe --dll C:\Tools\Il2CppLua.dll --lua C:\Scripts\startup.lua
```

连接成功后进入 `ilune >>`，可以直接输入 Lua：

```lua
print("hello from Il2CppLua")
print(il2cpp.get_status())
```

典型的类型查找、对象查找和字段读取流程：

```lua
local game = il2cpp.get_assembly("Assembly-CSharp")
local controller = game:get_class("", "BattleController")

il2cpp.schedule(function()
    local objects = controller:find_unity_objects()
    if not objects then
        print("find failed")
        return
    end

    for index, object in ipairs(objects) do
        print(index, object:get_class():get_full_name(), object:get_address())
        print("BattleID:", object:read_field("BattleID"))
    end
end)
```

`find_unity_objects()` 需要在 Unity 主线程调用，因此放在 `il2cpp.schedule()` 中。
字段名、方法名、命名空间和类型名必须替换为目标游戏中的实际 metadata。

<a name="api-reference"></a>
## 📚 Lua API

| 模块 | 说明 |
| --- | --- |
| [`lua`](#api-lua) | Lua table 辅助函数 |
| [`il2cpp`](#api-il2cpp) | IL2CPP 运行时入口、查找与调度 |
| [`Assembly`](#api-assembly) | 程序集查询 |
| [`Class`](#api-class) | 类型查询、对象创建与静态成员操作 |
| [`Instance`](#api-instance) | 实例、数组与 `List<T>` 操作 |
| [`Method`](#api-method) | 方法信息、调用与 Hook |
| [`Field`](#api-field) | 字段信息、读取与写入 |

<a name="api-lua"></a>
### 🧰 `lua`

<a name="lua-each"></a>
#### 🔁 `lua.each(table, callback)`

遍历 Lua table。回调参数为 `value, key`。

```lua
lua.each({ "a", "b", "c" }, function(value, key)
    print(key, value)
end)

lua.each({ hp = 100, mp = 50 }, function(value, key)
    print(key, value)
end)
```

<a name="lua-dump"></a>
#### 🧾 `lua.dump(table)`

输出 Lua table 的第一层键值，不递归展开嵌套 table。

```lua
lua.dump({ name = "Player", stats = { hp = 100 } })
```

<a name="lua-hex"></a>
#### 🔢 `lua.hex(value)`

将整数或地址格式化为十六进制字符串。它不改变原值。

```lua
print(lua.hex(24))
print(lua.hex(instance:get_address()))
print(lua.hex(field:get_offset()))
```

<a name="api-il2cpp"></a>
### ⚙️ `il2cpp`

| API | 返回值或作用 |
| --- | --- |
| `il2cpp.get_status()` | 返回运行时、程序集、调度器、模块和日志状态文本 |
| `il2cpp.is_initialized()` | 返回 IL2CPP 是否初始化完成 |
| `il2cpp.get_assemblies()` | 返回全部程序集 table |
| `il2cpp.get_assembly(name)` | 返回指定 Assembly，找不到时返回 `nil` |
| `il2cpp.get_class(namespace, name)` | 跨程序集查找 Class，找不到时返回 `nil` |
| `il2cpp.wrap(address)` | 校验地址并返回 Instance，失败时返回 `nil, error` |
| `il2cpp.unhook_all()` | 移除用户安装的全部 Hook |
| `il2cpp.schedule(callback)` | 将无参 Lua 回调加入 tick 任务队列 |
| `il2cpp.set_tick(method)` | 设置任务调度使用的 tick 方法 |
| `il2cpp.get_tick()` | 返回 tick 方法签名，未设置时返回 `nil` |
| `il2cpp.is_tick_ready()` | 返回 tick Hook 是否已经安装 |

<a name="il2cpp-status"></a>
#### 📊 `il2cpp.get_status()`

返回多行状态文本，包括初始化状态、导出函数解析情况、程序集数量、镜像数量、tick 状态、
`GameAssembly.dll` 模块基址，以及日志队列的拒绝和丢弃统计。

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
Rejected log batches: 0
Discarded log frames: 0
Dropped log bytes (lower bound): 0
```

<a name="il2cpp-is-initialized"></a>
#### ✅ `il2cpp.is_initialized()`

返回运行时是否完成初始化。

```lua
assert(il2cpp.is_initialized(), "IL2CPP runtime is not ready")
```

<a name="il2cpp-assemblies"></a>
#### 🔎 `il2cpp.get_assemblies()` / `il2cpp.get_assembly(name)`

`get_assemblies()` 返回全部已加载程序集。`get_assembly(name)` 按名称查找程序集，名称比较忽略大小写，
也可省略 `.dll` 后缀。

```lua
lua.each(il2cpp.get_assemblies(), function(assembly)
    print(assembly:get_name())
end)

local game = il2cpp.get_assembly("Assembly-CSharp")
local core = il2cpp.get_assembly("mscorlib.dll")
print(game, core)
```

<a name="il2cpp-class"></a>
#### 🧬 `il2cpp.get_class(namespace, name)`

在全部已加载程序集内查找第一个匹配的 Class。存在同名类型时，使用
`assembly:get_class(namespace, name)` 指定程序集。

```lua
local player = il2cpp.get_class("Game", "Player")
local globalType = il2cpp.get_class("", "GlobalManager")
```

<a name="il2cpp-wrap"></a>
#### 📍 `il2cpp.wrap(address)`

接受整数或 `lightuserdata` 地址，并返回经过基础运行时校验的 Instance。

```lua
local object, err = il2cpp.wrap(0x000001ABCDEF1230)
if object then
    print(object:get_class():get_full_name())
else
    print("wrap failed:", err)
end
```

地址可能因对象销毁、内存复用或运行时状态变化而失效，不要长期保存未经验证的裸地址。

<a name="il2cpp-unhook-all"></a>
#### 🧹 `il2cpp.unhook_all()`

移除用户安装的全部 Hook。内部调度 tick 不属于用户 Hook。

```lua
il2cpp.unhook_all()
```

<a name="il2cpp-schedule"></a>
#### 🧵 `il2cpp.schedule(callback)`

将无参 Lua 回调加入任务队列。确认主线程后，回调在该线程的后续 tick 执行，不同步返回结果。

```lua
il2cpp.schedule(function()
    print("running on the detected Unity main thread")
end)
```

最多保留 1024 个尚未取出的任务；没有可用 tick 时仍允许有界排队。队列满或分配失败时抛出
`Il2Cpp Error`。

<a name="il2cpp-tick"></a>
#### ⏱️ `il2cpp.set_tick(method)`

使用具有原生地址的精确 Method 作为调度 tick。成功返回 `true`；失败返回 `false, error`。
应选择稳定、频繁且运行在 Unity 主线程的方法。切换 tick 不会改变已确认的主线程身份。
如果主线程探针无法安装，设置失败；已入队任务保留等待。

静态方法和实例方法都可以作为 tick。重复设置同一个 Method、切回曾经使用过的 Method，或选择
已经安装用户 Hook 的 Method 时，会复用现有原生 Hook；用户回调与调度 tick 可以共存。

```lua
local time = il2cpp.get_class("UnityEngine", "Time")
local ok, err = il2cpp.set_tick(time:get_method("get_deltaTime"))
if not ok then
    print("set tick failed:", err)
end

local loop = il2cpp.get_class("Game", "GameLoop")
assert(il2cpp.set_tick(loop:get_method("Update", "System.Single")))
```

`il2cpp.get_tick()` 返回当前 tick 的完整方法签名；尚未安装时返回 `nil`。
`il2cpp.is_tick_ready()` 返回 tick Hook 是否已经安装。

初始化时会依次尝试 `UnityEngine.Time.get_deltaTime`、`get_frameCount` 和
`UnityEngine.Object.get_name`。默认入口不适用时，再使用 `set_tick` 替换。

<a name="api-assembly"></a>
### 📦 `Assembly`

Assembly 表示一个已加载的 IL2CPP 程序集。

| API | 作用 |
| --- | --- |
| `assembly:get_name()` | 返回程序集镜像名称 |
| `assembly:get_class(namespace, name)` | 在该程序集内查找 Class |
| `assembly:get_classes()` | 返回该程序集声明的全部 Class |

```lua
local game = il2cpp.get_assembly("Assembly-CSharp")
print(game:get_name())

local player = game:get_class("Game", "Player")
local global = game:get_class("", "GlobalManager")

for _, class in ipairs(game:get_classes()) do
    print(class:get_full_name())
end
```

`assembly:get_class()` 只在当前程序集内查找，不回退到全程序集搜索。

<a name="api-class"></a>
### 🧬 `Class`

Class 表示一个 IL2CPP 类型。

| API 区域 | 说明 |
| --- | --- |
| [类型信息](#class-information) | 查看名称、继承关系、大小和地址 |
| [方法查找](#class-methods) | 查找方法和重载 |
| [字段查找](#class-fields) | 查找字段 |
| [创建对象](#class-new) | 分配并构造对象 |
| [创建数组](#class-array) | 创建一维托管数组 |
| [静态方法](#class-static-call) | 调用静态方法 |
| [静态字段](#class-static-field) | 读取和写入静态字段 |
| [Unity 对象查找](#class-find-unity-objects) | 查询场景中的 Unity 对象 |
| [类型 dump](#class-dump) | 输出类型信息 |

<a name="class-information"></a>
#### 📋 类型信息

| API | 返回值 |
| --- | --- |
| `class:get_name()` | 类型名 |
| `class:get_namespace()` | 命名空间 |
| `class:get_full_name()` | 完整类型名 |
| `class:get_assembly()` | 声明该类型的 Assembly |
| `class:get_parent()` | 父类 Class，没有父类时为 `nil` |
| `class:is_value_type()` | 是否为值类型 |
| `class:is_enum()` | 是否为枚举 |
| `class:get_instance_size()` | 实例大小，无法取得时为 `nil` |
| `class:get_address()` | `Il2CppClass` 地址 |
| `class:dump()` | 输出类型信息 |

```lua
local class = il2cpp.get_class("Game", "Player")
print(class:get_name())
print(class:get_namespace())
print(class:get_full_name())
print(class:get_assembly():get_name())
print(class:get_parent())
print(class:is_value_type(), class:is_enum())
print(class:get_instance_size())
print(lua.hex(class:get_address()))
class:dump()
```

<a name="class-methods"></a>
#### 🔍 方法查找

```lua
local update = class:get_method("Update")
local byId = class:get_method("FindItem", "int")
local byName = class:get_method("FindItem", "System.String")

for _, method in ipairs(class:get_methods()) do
    print(method:get_signature())
end
```

`get_method(name)` 返回第一个同名方法。存在重载时传入参数类型进行精确查找：

```lua
local threeStrings = class:get_method("Find", "string", "string", "string")
local stringArray = class:get_method("Find", "string[]")
```

支持完整 IL2CPP 类型名、末级类型名、基础别名和一维数组后缀。常用别名包括 `bool`、`int`、
`uint`、`long`、`float`、`double`、`string` 和 `object`。

普通方法查找会沿父类查找；构造函数 `.ctor` 和类型初始化方法 `.cctor` 只在声明类查找。
`get_methods()` 只枚举该 Class 声明的方法。

<a name="class-fields"></a>
#### 🏷️ 字段查找

```lua
local health = class:get_field("health")
print(health:get_signature())

for _, field in ipairs(class:get_fields()) do
    print(field:get_signature())
end
```

`get_field()` 和 `get_fields()` 只处理该 Class 声明的字段。Instance 字段读写以及 Class 的静态字段
便捷接口会沿父类查找。

<a name="class-new"></a>
#### 🆕 创建对象

```lua
local object = class:new(arg1, arg2)
local raw = class:alloc()
```

`new()` 会查找匹配的构造函数并执行；`alloc()` 只分配对象，不执行构造函数。值类型无参数且没有显式
构造函数时，`new()` 返回默认值对象。引用类型必须找到可匹配的构造函数。

<a name="class-array"></a>
#### 🧱 创建数组

```lua
local intClass = il2cpp.get_class("System", "Int32")
local values = intClass:new_array(3)
values[1], values[2], values[3] = 10, 20, 30
print(#values)

local objects = class:new_array(2)
objects[1] = class:new()
objects[2] = nil
```

数组使用 Lua 1 基索引，长度必须是非负整数。

<a name="class-static-call"></a>
#### ⚡ 静态方法

```lua
local manager = il2cpp.get_class("Game", "PlayerManager")
print(manager:static_call("GetCurrent"))
manager:static_call("SetDifficulty", 2)
```

`static_call()` 会根据 Lua 参数选择静态方法重载，并沿父类查找。

<a name="class-static-field"></a>
#### 🗂️ 静态字段

```lua
local manager = il2cpp.get_class("Game", "PlayerManager")
print(manager:read_static_field("Instance"))
manager:write_static_field("DebugEnabled", true)
```

`read_static_field()` 和 `write_static_field()` 会沿父类查找静态字段。常量字段不可写。

<a name="class-find-unity-objects"></a>
#### 🎮 Unity 对象查找

```lua
local enemy = il2cpp.get_class("Game", "EnemyController")

il2cpp.schedule(function()
    local enemies, err = enemy:find_unity_objects()
    if not enemies then
        print("find failed:", err)
        return
    end

    for index, object in ipairs(enemies) do
        print(index, object:get_address())
    end
end)
```

查询类型必须继承 `UnityEngine.Object`。返回值是 Instance table；没有匹配对象时返回空 table，
查询失败时返回 `nil, error`。

<a name="class-dump"></a>
#### 🧾 `class:dump()`

输出当前类的程序集、类型种类、父类、地址、实例大小，以及当前类声明的字段和方法。
展示最多保留 1024 个字段和 1024 个方法，超过时会显示截断提示；文本总长度也有上限。

<a name="api-instance"></a>
### 🎮 `Instance`

Instance 表示一个托管对象，也用于表示数组、`List<T>` 和装箱后的值类型。

| API 区域 | 说明 |
| --- | --- |
| [实例信息](#instance-information) | 查看实例类型、地址和字段 |
| [实例方法](#instance-methods) | 调用实例方法 |
| [实例字段](#instance-fields) | 读取和写入实例字段 |
| [数组与 List](#instance-containers) | 访问数组和 `List<T>` |

<a name="instance-information"></a>
#### 📋 实例信息

```lua
print(object:get_class():get_full_name())
print(lua.hex(object:get_address()))
print(object)
```

| API | 作用 |
| --- | --- |
| `instance:get_class()` | 返回实际类型 Class |
| `instance:get_address()` | 返回托管对象地址 |
| `instance:dump()` | 输出实例字段 |
| `instance:dump(true)` | 连同父类实例字段一起输出 |

<a name="instance-methods"></a>
#### 📞 实例方法

```lua
local player = class:new()
player:call("SetLevel", 20)
print(player:call("GetLevel"))

local getLevel = class:get_method("GetLevel")
print(getLevel:call(player))
```

`instance:call(name, ...)` 根据 Lua 参数选择实例方法重载。`void` 方法没有 Lua 返回值。

<a name="instance-fields"></a>
#### 📝 实例字段

```lua
print(player:read_field("health"))
player:write_field("health", 999)
player:write_field("target", nil)
```

静态字段必须使用 `Field:read/write` 或 `Class:read_static_field/write_static_field`。

<a name="instance-containers"></a>
#### 📚 数组与 List

数组和 `List<T>` 使用 Lua 1 基索引：

```lua
print(#items)
print(items[1])
items[1] = items[2]

items:each(function(value, index)
    print(index, value)
end)

items:dump()
```

普通对象不支持长度运算或数字下标。容器越界、非整数下标和多维数组会抛出错误。

<a name="api-method"></a>
### 🔧 `Method`

| API | 作用 |
| --- | --- |
| `method:get_name()` | 方法名 |
| `method:get_class()` | 声明方法的 Class |
| `method:get_signature()` | 方法签名 |
| `method:get_address()` | 已编译的原生入口地址，无法取得时为 `nil` |
| `method:call(instance, ...)` | 精确调用方法 |
| `method:hook(callback)` | 安装 Lua Hook |
| `method:is_hooked()` | 查询 Hook 状态 |
| `method:unhook()` | 移除该方法的用户 Hook |

实例方法的 `call()` 需要先传 Instance；静态方法不传 Instance：

```lua
local getLevel = class:get_method("GetLevel")
local getCurrent = class:get_method("GetCurrent")

print(getLevel:call(player))
print(getCurrent:call())
```

`void` 方法返回 0 个 Lua 值；其他返回类型返回 1 个值，包括表示 null 引用的 `nil`。
普通调用允许闭合泛型方法，但拒绝开放泛型、`Nullable<T>`、`ref/out` 参数和 byref 返回值。

<a name="method-hook"></a>
#### 🪝 `method:hook(callback)`

实例方法回调格式为 `function(this, original, ...)`：

```lua
local damage = class:get_method("TakeDamage", "float")
damage:hook(function(this, original, amount)
    print("TakeDamage", amount)
    return original(amount * 0.5)
end)
```

静态方法回调的第一个参数是声明该方法的 Class：

```lua
local calculate = class:get_method("CalculateScore", "int")
calculate:hook(function(declaringClass, original, value)
    print(declaringClass:get_full_name(), value)
    return original(value) * 2
end)
```

`original()` 不传参数时透传本次 Hook 的原始参数；传入参数时使用新参数调用原方法。回调也可以
不调用 `original()`，直接返回替代结果：

```lua
damage:hook(function(this, original, amount)
    return 0
end)
```

发生回调错误或返回值不兼容时，如果尚未调用 `original`，桥接会尝试调用原方法一次；如果已经
调用过，则复用已完成的结果，不重复原生副作用。

<a name="method-hook-state"></a>
#### 🔁 `method:is_hooked()` / `method:unhook()`

```lua
if damage:is_hooked() then
    damage:unhook()
end

il2cpp.unhook_all()
```

<a name="api-field"></a>
### 🏷️ `Field`

| API | 作用 |
| --- | --- |
| `field:get_name()` | 字段名 |
| `field:get_class()` | 声明字段的 Class |
| `field:get_signature()` | 字段类型签名 |
| `field:get_offset()` | 实例字段偏移；静态字段返回 `nil` |
| `field:read(instance)` | 读取字段 |
| `field:write(instance, value)` | 写入字段 |

实例字段传入 Instance：

```lua
local field = class:get_field("health")
print(field:get_name())
print(field:get_signature())
print(lua.hex(field:get_offset()))
print(field:read(player))
field:write(player, 500)
```

静态字段不传 Instance：

```lua
local instanceField = class:get_field("Instance")
print(instanceField:read())
instanceField:write(player)
```

常量字段不可写。值类型字段可以写入装箱后的值类型 Instance；传入 `nil` 表示值类型默认零值。

<a name="type-mapping"></a>
## 🔄 类型映射

| IL2CPP 类型 | Lua 类型 |
| --- | --- |
| `bool` | boolean |
| 整数、enum、char | integer |
| `float`、`double` | number |
| `System.String` | string 或 `nil` |
| class、object、array | Instance 或 `nil` |
| `IntPtr`、`UIntPtr` | integer |
| 原生指针、函数指针 | lightuserdata |
| struct | 装箱后的 Instance |

Lua integer 是有符号 64 位。读取大于 `INT64_MAX` 的 `ulong` 时会按原始位模式表现为负数；
窄整数、`char` 和枚举会进行范围检查，越界不会静默截断。`UInt64`、`UIntPtr` 及 64 位无符号
枚举仍接受完整的负数位模式。

引用类型参数和字段可以使用兼容的 Instance 或 `nil`。struct 参数、字段和数组元素可以使用
装箱后的值类型 Instance 或 `nil` 默认值。

<a name="hook-threading"></a>
## 🪝 线程与 Hook

### 🧵 Unity 主线程

Il2CppLua 命令可以从控制线程执行，但 Unity API 和 Unity 对象应在 Unity 主线程使用：

```lua
il2cpp.schedule(function()
    local gameObject = il2cpp.get_class("UnityEngine", "GameObject")
    local objects, err = gameObject:find_unity_objects()
    if objects then
        print(#objects)
    else
        print(err)
    end
end)
```

`il2cpp.schedule()` 只在已确认的主线程命中选定 tick 时执行任务。主线程身份由独立的一次性
`UnitySynchronizationContext.ExecuteTasks` 探针建立；无法安装时回退到 `Time.get_deltaTime`。
探针只接受非嵌套原生调用，控制台主动调用和 Lua Hook 内的调用不能注册主线程。
确认前任务保持排队，`set_tick()` 不会清除或重新绑定线程身份。
`is_tick_ready()` 仅表示 tick Hook 已安装，不表示探针已确认主线程。

### 🪝 Hook 回调

Hook 回调可能在任意游戏线程执行；进入 Lua 前会附加当前 IL2CPP 线程。高频 Hook 中不要进行大量
打印、文件 IO 或长时间 Lua 计算。

`original()` 直接调用原方法 trampoline，省略参数时沿用捕获的原始参数，传入参数时使用统一类型
转换规则。值类型 Hook 的 `this` 是供 Lua 查看或操作的装箱快照，修改快照不会写回原生 `this`。

访问违例等原生故障会永久隔离当前 Lua 会话。隔离后不再进入 Lua VM，后续 Hook 直接透传原方法，
命令会报告会话不可用；必须重启目标进程才能恢复。普通 Lua 错误和 `runtime_invoke` 正常报告的
托管异常不触发隔离。

卸载时会先禁用 Hook，并等待正在执行的 detour 和原方法回退完成。只能在 Windows x64、标准
IL2CPP 调用约定下 Hook；不支持泛型方法、实例化泛型方法、`ref/out`、byref 返回和无法表示的
结构体 ABI，方法最多 64 个声明参数。

<a name="protocol-errors"></a>
## 📡 通信、错误与输出

Lune 与 DLL 使用命名管道，并严格校验握手字符串：

```text
MSG_HELLO: Il2CppLua/4.1.1
```

版本不匹配时，Lune 会显示 `expected` 和 `received`，并在等待 READY 或进入 REPL 前终止连接。

错误会按 `Lua Error`、`Il2Cpp Error`、`CSharp Error` 和 `Lune Error` 分类；Lua 文件错误可以带
源文件行号。命名管道单帧负载上限为 1 MiB，写入超时会取消当前 I/O。

单次 `print`、REPL 返回值回显以及每个命令、Hook、schedule 的输出捕获批次最多保留 1 MiB，
超出时显示 `[output truncated]`。日志按 UTF-8 边界分成不超过 64 KiB 的帧，待发送队列最多 1024
帧和 4 MiB；队列满或连接停止时，日志可能被拒绝或丢弃，计数可通过 `il2cpp.get_status()` 查看。

<a name="lune-cli"></a>
## 💻 Lune 命令行

```text
Lune.exe -i -n <进程名> [-d <DLL路径>] [-l <Lua脚本>]
Lune.exe -i -p <PID>    [-d <DLL路径>] [-l <Lua脚本>]
```

| 参数 | 说明 |
| --- | --- |
| `-i` | 使用 Il2CppLua IL2CPP 后端 |
| `-n`、`--name` | 按进程名查找并注入目标进程 |
| `-p`、`--pid` | 按 PID 注入目标进程 |
| `-d`、`--dll` | 指定 `Il2CppLua.dll` 路径；默认查找 Lune.exe 同目录 |
| `-l`、`--lua` | 连接并初始化后执行启动 Lua 脚本 |
| `-h`、`--help` | 显示帮助 |

表达式在 REPL 中会自动回显结果；语句、函数定义和控制流按原样执行。输入 `exit` 或 `quit` 结束会话。

启动脚本示例：

```bat
Lune.exe -i --name Game.exe --lua C:\Scripts\startup.lua
```

`--lua` 的相对路径以启动 Lune 的工作目录为基准。脚本由目标游戏进程读取，因此目标进程必须能够访问该路径。
在 REPL 中使用 `dofile()` 时，相对路径以游戏进程工作目录为基准；需要固定路径时使用绝对路径：

```lua
dofile([[C:\Scripts\test.lua]])
```

<a name="build"></a>
## 🔨 构建

构建环境：

- Windows x64。
- Visual Studio，包含 MSVC v145 工具集。
- Windows SDK 10.0。
- MASM x64 构建支持。

在 Visual Studio 中打开 `Il2CppLua.slnx`，选择 `Release | x64` 生成 `Il2CppLua.dll`。
`src/hook_stub.asm` 必须由 MASM 编译；`lua_src/` 和 `minhook_src/` 是随附的第三方源码，
业务修改应集中在 `src/`。

<a name="limitations"></a>
## ⚠️ 限制

- 仅支持 Windows x64 Unity IL2CPP 进程。
- 依赖目标运行时保留所需的 `il2cpp_*` 导出函数。
- 只能访问已经加载到目标进程的程序集、类型和对象；`find_unity_objects()` 不遍历完整托管堆。
- `get_class()` 遇到同名类型时返回第一个结果；`get_method(name)` 遇到重载时返回第一个结果，
  应传入参数类型进行精确选择。
- 开放泛型、实例化泛型 Hook、`Nullable<T>`、`ref/out` 参数和 byref 返回值不支持；闭合泛型的普通调用可以使用。
- 只支持一维零基 `SZARRAY`；多维数组的 bounds 和索引未实现。
- 含托管引用的 struct 数组元素暂不支持直接写入。
- Hook 只接受标准 IL2CPP Windows x64 调用约定，方法最多 64 个声明参数；值类型 Hook 的 Lua `this`
  是快照，修改不会写回原生值。
- `Time.get_deltaTime` 回退探针依赖游戏在主线程调用该入口的惯例；无法像 `ExecuteTasks` 一样提供明确的 PlayerLoop 线程语义。
- 任意裸地址即使当前可读，也可能在之后因对象销毁、内存复用或运行时状态变化而失效。
- 发生原生故障后当前 Lua 会话会被永久隔离，必须重启目标进程恢复。

<a name="license"></a>
## 📄 License

MIT License，详见 [LICENSE](LICENSE)。
