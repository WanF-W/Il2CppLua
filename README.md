<div align="center">

# 🔧 Il2CppLua

在 Unity IL2CPP 游戏里用 Lua 读写数据、调用方法、Hook 逻辑

**v2.1.0** · Windows x64 · Unity 2018.3+ · MIT

</div>

## 📑 目录

- [简介](#intro)
- [特性](#features)
- [快速开始](#quickstart)
- [API 参考](#api)
  - [全局函数 `il2cpp.*`](#api-global)
  - [Class 元表](#api-class)
  - [Instance 元表](#api-instance)
  - [Method 元表](#api-method)
  - [Field 元表](#api-field)
  - [主线程调度 `il2cpp.mainThread`](#api-mainthread)
- [Hook 使用](#hook)
- [线程模型](#threading)
- [构建](#build)
- [已知限制](#limits)
- [License](#license)

<a id="intro"></a>
## 简介

Il2CppLua 是一个注入到 Unity IL2CPP 游戏进程内的原生 DLL。它动态解析
GameAssembly.dll 的 `il2cpp_*` 导出函数，把 IL2CPP 的类、对象、方法、字段
暴露给内嵌的 Lua 5.4 虚拟机，并通过 MinHook 提供方法级 Hook。

配套 CLI 工具是 [ILune](../ILune)，负责 DLL 注入与交互式控制台，两个项目
必须配合使用。

<a id="features"></a>
## ✨ 特性

- 动态解析 GameAssembly.dll 导出函数，不依赖外部 SDK 或 dump 头文件
- 内嵌 Lua 5.4.8，完整标准库，`print` 输出实时回显
- Class / Instance / Method / Field 四种 userdata，风格参考 frida-il2cpp-bridge
- 实例 / 静态字段读写，实例 / 静态方法调用
- `call` / `static_call` 按名称自动匹配重载
- MinHook 方法级 Hook，回调内可调用 `original()` 执行原方法
- `il2cpp.mainThread.schedule` 把 Lua 代码投递到 Unity 主线程执行
- `il2cpp.each` / `il2cpp.dump` 通用遍历：Lua 表、数组、`List<T>`
- 数组下标读写与 `#` 长度，`List<T>` 同样支持
- 命名管道 IPC，带版本握手与超时保护

<a id="quickstart"></a>
## 🚀 快速开始

1. 编译 `Il2CppLua.dll` 与 `ilune.exe`，放在同一目录
2. 启动游戏，运行 `ilune -n Game.exe`
3. 在 `ilune >>` 提示符后输入 Lua

```lua
local hero = il2cpp.get_class("MyGame", "HeroData")
local obj  = hero:new()

obj:call("SetLevel", 10)
print(obj:call("GetLevel"))

obj:set("hp", 100)
print(obj:get("hp"))
```

<a id="api"></a>
## 📖 API 参考

<a id="api-global"></a>
### 全局函数 `il2cpp.*`

| API | 说明 |
| --- | --- |
| `get_class(ns, name)` | 查找类，返回 `Class` 或 `nil` |
| `get_status()` | 导出函数解析状态 |
| `is_initialized()` | 是否初始化完成 |
| `get_assemblies()` / `get_image_count()` | 程序集 / 镜像数量 |
| `wrap(address)` | 裸指针包装为 `Instance` |
| `find_objects(klass)` | 查找该类型的存活对象 |
| `each(container, fn)` | 通用遍历，回调 `fn(value, index)` |
| `dump(container)` | 输出容器长度与全部元素 |
| `unhook_all()` | 卸载全部用户 Hook |

```lua
-- get_class：最常用的入口
local hero = il2cpp.get_class("MyGame", "HeroData")

-- 状态查询
print(il2cpp.get_status())
print(il2cpp.is_initialized())
print(il2cpp.get_assemblies(), il2cpp.get_image_count())

-- wrap：把裸指针包成 Instance
local obj = il2cpp.wrap(0x7FF600001234)

-- find_objects + each + dump
local objs = il2cpp.find_objects(hero)
il2cpp.each(objs, function(o, i) print(i, o) end)
il2cpp.dump(objs)

-- 清理
il2cpp.unhook_all()
```

`each` / `dump` 支持三种容器：Lua 表、IL2CPP 数组、`List<T>`。

<a id="api-class"></a>
### Class 元表

| API | 说明 |
| --- | --- |
| `get_name()` / `get_namespace()` / `get_parent()` | 类名、命名空间、父类 |
| `get_method(name)` / `get_methods()` | 查找单个 / 全部方法 |
| `get_field(name)` / `get_fields()` | 查找单个 / 全部字段 |
| `new(...)` | 创建对象并按参数个数匹配构造函数 |
| `static_call(name, ...)` | 调用静态方法（自动匹配重载） |
| `static_get(name)` / `static_set(name, v)` | 读写静态字段 |
| `find_objects()` | 查找该类型的存活对象 |
| `get_instance_size()` / `get_address()` | 实例大小 / 类地址 |

```lua
local cls = il2cpp.get_class("MyGame", "HeroData")

-- 类信息
print(cls:get_name(), cls:get_namespace())
print(cls:get_parent())

-- 方法 / 字段
local mth  = cls:get_method("GetLevel")
local fld  = cls:get_field("hp")
local mths = cls:get_methods()
local flds = cls:get_fields()

-- 创建对象
local obj = cls:new()

-- 静态成员
cls:static_call("Reset")
cls:static_set("InstanceCount", 1)
print(cls:static_get("InstanceCount"))

-- 对象查找 / 信息
il2cpp.dump(cls:find_objects())
print(cls:get_instance_size())
print(string.format("0x%X", cls:get_address()))
```

<a id="api-instance"></a>
### Instance 元表

| API | 说明 |
| --- | --- |
| `call(name, ...)` | 调用实例方法（自动匹配重载） |
| `get(name)` / `set(name, v)` | 读写实例字段 |
| `get_class()` / `get_address()` | 类 / 对象地址 |
| `each(fn)` | 遍历数组或 `List<T>` |
| `obj[i]` / `obj[i] = v` | 数组元素读写（Lua 索引从 1 开始） |
| `#obj` | 数组 / `List<T>` 长度 |

```lua
local obj = hero:new()

-- call / get / set
local lv = obj:call("GetLevel")
obj:call("SetLevel", lv + 1)
obj:set("hp", 100)
print(obj:get("hp"))

-- 信息
print(obj:get_class():get_name())
print(string.format("0x%X", obj:get_address()))

-- T[] 数组
local arr = obj:get("buffers")
print(#arr, arr[1])
arr[1] = 999

-- List<T>：长度、遍历、按下标取元素
local lst = obj:get("itemList")
print(#lst)
lst:each(function(item, i) print(i, item) end)
print(lst:call("get_Item", 0))
```

<a id="api-method"></a>
### Method 元表

| API | 说明 |
| --- | --- |
| `get_name()` / `get_param_count()` | 方法名 / 参数个数 |
| `get_return_type()` / `get_params()` | 返回类型 / 参数类型列表 |
| `is_static()` | 是否静态 |
| `call(obj, ...)` | 显式调用（静态传 `nil`） |
| `ovload(type1, ...)` | 按类型签名找重载 |
| `hook(fn)` / `unhook()` / `hooked()` | Hook / 恢复 / 查询 |
| `get_address()` | 方法原生地址（methodPointer） |

```lua
local mth = cls:get_method("GetLevel")

-- 方法信息
print(mth:get_name(), mth:get_param_count())
print(mth:get_return_type())
il2cpp.dump(mth:get_params())
print(mth:is_static())
print(string.format("0x%X", mth:get_address()))

-- 显式调用：实例方法传 Instance
print(mth:call(obj))

-- 按类型签名找重载再调用
local setter = cls:get_method("SetLevel"):ovload("int")
setter:call(obj, 99)

-- Hook（详见下文）
mth:hook(function(this, original) return original() + 1 end)
print(mth:hooked())
mth:unhook()
```

`ovload` 类型字符串：`bool / byte / sbyte / short / ushort / int / uint /
long / ulong / float / double / char / string / object / void`，或类名
（支持 `"Namespace.ClassName"` 格式）。

<a id="api-field"></a>
### Field 元表

| API | 说明 |
| --- | --- |
| `get_name()` / `get_type()` | 字段名 / 类型 |
| `get_offset()` | 字段在对象内的字节偏移 |
| `get(obj)` / `set(obj, v)` | 读写字段（静态传 `nil`） |

```lua
local fld = cls:get_field("hp")
print(fld:get_name(), fld:get_type(), fld:get_offset())

-- 实例字段
print(fld:get(obj))
fld:set(obj, 500)

-- 静态字段
local countFld = cls:get_field("InstanceCount")
countFld:set(nil, 2)
print(countFld:get(nil))
```

<a id="api-mainthread"></a>
### 主线程调度 `il2cpp.mainThread`

| API | 说明 |
| --- | --- |
| `schedule(fn)` | 投递到 Unity 主线程执行 |
| `set_tick(ns, class, method)` | 指定内部 tick 入口 |
| `get_tick()` | 查询当前 tick 入口 |
| `is_ready()` | 内部 tick 是否已安装 |

```lua
il2cpp.mainThread.schedule(function()
    print("在 Unity 主线程执行")
end)

-- 默认入口不可用时手动指定
print(il2cpp.mainThread.set_tick("UnityEngine", "Time", "get_deltaTime"))
print(il2cpp.mainThread.get_tick())
print(il2cpp.mainThread.is_ready())
```

内部 tick 默认依次尝试 `Time.get_deltaTime` / `Time.get_frameCount` /
`Object.get_name`，初始化时自动预装。

<a id="hook"></a>
### Hook 使用

```lua
local mth = cls:get_method("Damage")

mth:hook(function(this, original, amount)
    print("Damage:", amount)
    -- 不调用 original：直接替换返回值
    -- return 0
    -- 调用 original：透传原始参数或替换参数
    return original(amount * 2)
end)

mth:unhook()
```

回调签名与 frida-il2cpp-bridge 一致：

- 实例方法：`function(this, original, ...)`
- 静态方法：`function(Class, original, ...)`
- `original()` 透传原始参数，`original(替换参数...)` 替换参数
- 回调返回值作为方法返回值（void 方法忽略）

<a id="threading"></a>
## 🧵 线程模型

- Lua 状态机由可重入互斥锁串行化，Hook 回调内可再次调用被 Hook 的方法
- Hook 回调可能发生在任意游戏线程，分发器先附加 IL2CPP 线程再进 Lua
- `schedule` 任务由 Unity 主线程执行
- 卸载 Hook 只禁用不释放 trampoline，避免在途回调悬空

<a id="build"></a>
## 🔨 构建

环境要求：Windows x64、Visual Studio 2022（工具集 v145）、Windows SDK 10.0、
MASM（编译 `hook_stub.asm`，VS 自带）。

```bat
:: 打开 Il2CppLua.slnx，选择 Release | x64 生成
:: 产物：Il2CppLua.dll
```

> Debug / Win32 配置是调试宿主程序，正式使用请用 Release | x64。

<a id="limits"></a>
## ⚠️ 已知限制

- 仅 Windows x64，假定 Unity 2018.3+
- `ref/out` 参数只读，回调内修改不会写回
- 共享 methodPointer 的泛型实例化方法只能 Hook 其中一个
- `get_method(name)` 返回第一个同名方法，精确重载用 `ovload`
- 方法枚举上限 1024 个

<a id="license"></a>
## 📄 License

MIT License，详见 [LICENSE](LICENSE)。
