# Il2CppLua

> A native C++ bridge for Lua interaction with Il2Cpp — v2.1.0

## 简介

Il2CppLua 是一个注入到 Unity IL2CPP 游戏进程内的原生 DLL。它动态解析
GameAssembly.dll 的 `il2cpp_*` 导出函数，把 IL2CPP 的类 / 对象 / 方法 / 字段
暴露给内嵌的 Lua 5.4 虚拟机，让你可以用 Lua 脚本直接读写游戏数据、调用方法，
并通过 MinHook 对游戏方法做原生级 Hook。配套的 CLI 工具是 ILune（负责注入与
管道交互控制台），两个项目必须配合使用。

## 特性

- 动态解析 GameAssembly.dll 导出函数，不依赖任何外部 SDK 或 dump 头文件
- 内嵌 Lua 5.4.8，完整标准库，`print` 输出重定向到 CLI 控制台
- Class / Instance / Method / Field 四种 userdata，接口风格参考 frida-il2cpp-bridge
- 实例字段 / 静态字段读写，实例方法 / 静态方法调用
- 按名称调用时自动匹配重载（参数个数 + Lua 实参类型兼容性打分）
- 方法级 Hook（MinHook）：回调内可调用 `original()` 执行原方法、替换参数或返回值
- 主线程调度 `il2cpp.mainThread.schedule`：把 Lua 代码投递到 Unity 主线程执行
- 通用遍历 `il2cpp.each` / `il2cpp.dump`：支持 Lua 表、IL2CPP 数组、`List<T>`
- 数组元素读写（`obj[i]`、`#obj`），`List<T>` 长度与遍历
- 通过 Windows 命名管道与 CLI 通信：命令执行、实时输出、错误回传、版本握手
- 仅 Windows x64 + Unity 2018.3+ IL2CPP

## 工作原理

1. ILune 通过 `CreateRemoteThread + LoadLibraryW`（失败时回退 `NtCreateThreadEx`）
   把本 DLL 注入目标游戏进程，并通过共享内存传递管道名称。
2. DLL 的工作线程连接命名管道、发送 HELLO 握手。
3. 工作线程重试等待 GameAssembly.dll 加载，随后解析全部 `il2cpp_*` 导出函数、
   附加当前线程、缓存所有程序集 Image。
4. 创建 Lua 虚拟机并注册桥接层（`LuaBridge_Init`），发送 READY 通知 CLI。
5. 进入消息循环：收到 CMD / FILE 帧就执行 Lua 代码，把 `print` 输出、返回值
   回显、错误信息通过 LOG / ERROR / OK 帧回传。

所有 Lua 访问由一把可重入互斥锁串行化；Hook 回调可能发生在任意游戏线程，
分发器会先 `il2cpp_thread_attach` 再进入 Lua，保证调用 IL2CPP API 合法。

## 目录结构

```
src/
  dll_main.cpp         DLL 入口、工作线程、消息循环
  il2cpp_resolver.*    GameAssembly.dll 导出函数解析与封装
  lua_bridge.*         Lua ↔ IL2CPP 桥接（userdata 元表、类型编组）
  lua_engine.*         Lua 虚拟机管理、print 重定向、返回值回显
  il2cpp_hook.*        MinHook 方法 Hook 与主线程调度
  hook_stub.asm        Hook 共用汇编跳板（x64 MASM）
  pipe_channel.*       DLL 侧命名管道客户端
  protocol.h           两端共享的通信协议定义
lua_src/               内嵌 Lua 5.4.8 源码
minhook_src/           MinHook 源码（x86/x64）
```

## 构建

环境要求：

- Windows x64
- Visual Studio 2022（平台工具集 v145）
- Windows SDK 10.0
- MASM（ml64，用于编译 `hook_stub.asm`，VS 自带）

步骤：

1. 打开 `Il2CppLua.slnx`（或直接打开 `Il2CppLua.vcxproj`）。
2. 配置选择 **Release | x64**。
3. 生成解决方案，产物为 `Il2CppLua.dll`。

> 注意：Debug / Win32 配置是调试用宿主程序，正式使用请使用 Release|x64
> （该配置输出 DLL）。

## 快速开始

1. 编译出 `Il2CppLua.dll` 与 `ILune` 项目的 `ilune.exe`，放在同一目录。
2. 启动目标游戏。
3. 运行 `ilune -n Game.exe`（按进程名）或 `ilune -p 1234`（按 PID）。
4. 在 `ilune >>` 提示符后直接输入 Lua 代码。

```lua
-- 查找类
local hero = il2cpp.get_class("MyGame", "HeroData")
print(hero:get_name(), hero:get_namespace())

-- 创建对象并调用方法（同名重载自动匹配）
local obj = hero:new()
obj:call("SetLevel", 10)
print(obj:call("GetLevel"))

-- 读写字段
print(obj:get("hp"))
obj:set("hp", 100)

-- 静态字段 / 静态方法
local count = hero:static_get("InstanceCount")
hero:static_call("Reset")

-- 查找某个类型的存活对象
local objs = il2cpp.find_objects(hero)
for i, o in ipairs(objs) do print(i, o) end
```

## Lua API 参考

### 全局函数（`il2cpp.*`）

| 函数 | 说明 |
| --- | --- |
| `get_status()` | 返回导出函数解析状态文本 |
| `get_class(namespace, name)` | 按命名空间 + 类名查找类，返回 Class 或 nil |
| `get_assemblies()` | 返回程序集（Image）数量 |
| `get_image_count()` | 返回镜像数量（同 get_assemblies） |
| `is_initialized()` | 桥接层是否已初始化 |
| `wrap(address)` | 把裸指针包装为 Instance（读对象头识别类） |
| `find_objects(klass)` | 通过 `FindObjectsOfType` 查找该类型的存活对象，返回 table |
| `each(container, fn)` | 通用遍历，回调 `fn(value, index)` |
| `dump(container)` | 输出容器长度与全部元素（`[index] = value`） |
| `unhook_all()` | 卸载全部用户 Hook |

`il2cpp.each` / `il2cpp.dump` 支持三种容器：Lua 表（先数组部分再键值部分）、
IL2CPP 数组、`System.Collections.Generic.List<T>`。

### Class 元表（`cls:...`）

| 方法 | 说明 |
| --- | --- |
| `get_name()` | 类名 |
| `get_namespace()` | 命名空间 |
| `get_parent()` | 父类，Class 或 nil |
| `get_method(name)` | 按名字找方法，返回 Method 或 nil（返回第一个同名方法） |
| `get_methods()` | 全部方法，返回 table of Method |
| `get_field(name)` | 按名字找字段，返回 Field 或 nil |
| `get_fields()` | 全部字段，返回 table of Field |
| `new(...)` | 分配对象并按参数个数匹配 `.ctor` 调用 |
| `static_call(name, ...)` | 调用静态方法（同名重载自动匹配） |
| `static_get(name)` | 读静态字段 |
| `static_set(name, value)` | 写静态字段 |
| `find_objects()` | 查找该类型的存活对象 |
| `get_instance_size()` | 对象实例大小（含对象头） |
| `get_address()` | 返回 `Il2CppClass*` 原始地址 |

### Instance 元表（`obj:...`）

| 方法 / 操作符 | 说明 |
| --- | --- |
| `call(name, ...)` | 按名字调用实例方法（同名重载自动匹配） |
| `get(name)` | 读实例字段 |
| `set(name, value)` | 写实例字段 |
| `get_class()` | 对象的类 |
| `get_address()` | 对象指针地址 |
| `each(fn)` | 遍历数组 / `List<T>`，回调 `fn(value, index)` |
| `obj[i]` | 读取数组元素（Lua 索引从 1 开始） |
| `obj[i] = v` | 写入数组元素 |
| `#obj` | 数组 / `List<T>` 长度 |

### Method 元表（`mth:...`）

| 方法 | 说明 |
| --- | --- |
| `get_name()` | 方法名 |
| `get_param_count()` | 参数个数 |
| `get_return_type()` | 返回值类型名 |
| `get_params()` | 参数类型名列表，table of string |
| `is_static()` | 是否静态方法 |
| `call(obj, ...)` | 显式调用（实例方法第一个参数传 Instance，静态方法传 nil） |
| `ovload(type1, ...)` | 按参数类型签名查找重载，返回 Method |
| `hook(function(this, original, ...) ... end)` | 替换方法实现 |
| `unhook()` | 恢复原始实现 |
| `hooked()` | 是否已 Hook |
| `get_address()` | 方法原生代码地址（methodPointer） |

`ovload` 的类型字符串：`bool / byte / sbyte / short / ushort / int / uint /
long / ulong / float / double / char / string / object / void`，或类名
（支持 `"Namespace.ClassName"` 格式）。

### Field 元表（`fld:...`）

| 方法 | 说明 |
| --- | --- |
| `get_name()` | 字段名 |
| `get_type()` | 字段类型名 |
| `get_offset()` | 字段在对象内的字节偏移 |
| `get(obj)` | 读字段（obj 传 Instance；静态字段传 nil） |
| `set(obj, value)` | 写字段（obj 传 Instance；静态字段传 nil） |

### 主线程调度（`il2cpp.mainThread`）

参考 frida-il2cpp-bridge 的 `Il2Cpp.mainThread.schedule`，把一段 Lua 代码投递到
Unity 主线程执行，从而安全调用只有主线程能做的操作。

| 方法 | 说明 |
| --- | --- |
| `schedule(fn)` | 把函数加入队列，主线程在下一个 tick 取出执行（无参数、无返回值） |
| `set_tick(namespace, class, method)` | 指定内部 tick 入口方法，返回 boolean |
| `get_tick()` | 返回当前 tick 入口的 namespace, class, method |
| `is_ready()` | 内部 tick hook 是否已安装 |

内部 tick hook 默认依次尝试 `Time.get_deltaTime` / `Time.get_frameCount` /
`Object.get_name`，初始化时自动预装；`schedule` 会惰性重试。若目标游戏不调用
这些默认入口，用 `set_tick` 指定一个每帧必调且只在主线程调用的方法。

```lua
il2cpp.mainThread.schedule(function()
    print("run on unity main thread")
end)
```

### Hook 示例

```lua
local mth = hero:get_method("Damage")
mth:hook(function(this, original, amount)
    print("Damage called, amount =", amount)
    -- 不调用 original: 返回值直接作为方法返回值（可替换返回值）
    -- 调用 original: 使用原始参数执行
    --   return original()           -- 原始参数
    --   return original(amount * 2) -- 替换参数
    return original(amount)
end)

-- 卸载
mth:unhook()
```

回调签名与 frida-il2cpp-bridge 一致：

- 实例方法：`function(this, original, 参数1, ...)`，`this` 为 Instance
- 静态方法：`function(Class, original, 参数1, ...)`，第一个参数为声明类
- `original()` 无参调用时透传原始参数；`original(替换参数...)` 使用替换参数
- 回调返回值会作为方法返回值（void 方法忽略）

## 线程模型

- Lua 状态机由一把可重入互斥锁保护，同一时刻只有一个线程执行 Lua 代码；
  Hook 回调内再次调用被 Hook 的方法不会死锁。
- Hook 回调可能发生在任意游戏线程，分发器自动附加 IL2CPP 线程后再进 Lua。
- `schedule` 的任务由 Unity 主线程执行，期间持有同一把 Lua 锁。
- 卸载 Hook 时只禁用不释放 trampoline，避免在途回调悬空。

## 已知限制

- 仅 Windows x64；假定 Unity 2018.3+（静态方法不再携带无用的 `__this` 参数）。
- `ref/out` 参数目前只读，回调内修改不会写回。
- 共享同一 methodPointer 的泛型实例化方法只能 Hook 其中一个 MethodInfo。
- `cls:get_method(name)` 返回第一个同名方法，精确重载请用
  `get_methods()` + `mth:ovload(...)` 或直接依赖 `call` 的自动匹配。
- 方法枚举上限 1024 个（`get_methods` / 重载匹配共用）。

## License

MIT License. 详见 [LICENSE](LICENSE)。
