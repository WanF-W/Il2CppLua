# Il2CppLua 4.1.1 Release Notes

## 版本信息

- Il2CppLua 版本：`4.1.1`
- 协议版本：`Il2CppLua/4.1.1`
- 配套组件：Il2CppLua.dll `4.1.1` 与包含 IL2CPP 后端 `4.1.1` 握手配置的 Lune
- 版本类型：PATCH。此次更新修正 Unity 主线程识别与 Scheduler tick 的内部协作，不新增 Lua API，不改变 Hook ABI 或通信协议格式。
- 整理依据：Il2CppLua 当前 `HEAD` 与工作区差异，以及 [`Optimization_MainThreadDetection.md`](Optimization_MainThreadDetection.md) 中的实施记录。
- 本次只进行源码和文档调整，未编译、未运行测试或启动游戏；下文描述的是已落到工作区的实现，不代表已经完成运行时验证。

<a name="version-and-pairing"></a>
## 1. 版本标识与配套要求

- [`src/version.h`](../src/version.h) 将 DLL 文件版本、产品版本和协议版本统一更新为 `4.1.1`，协议握手字符串为 `Il2CppLua/4.1.1`。
- Lune 的 [`src/backend_profile.h`](https://github.com/WanF-W/Lune/blob/master/src/backend_profile.h) 与 [`README.md`](https://github.com/WanF-W/Lune/blob/master/README.md) 同步使用 IL2CPP 后端版本 `4.1.1` 和握手标识 `Il2CppLua/4.1.1`。
- Il2CppLua.dll 与 Lune 的 IL2CPP 后端必须配套使用；版本或握手字符串不一致时，连接会在进入 READY 或 REPL 前终止。Lune 自身的产品版本不随本次 PATCH 变化。

<a name="main-thread-scheduler"></a>
## 2. Unity 主线程调度与检测

- `il2cpp.schedule(function)` 仍将无参 Lua 回调放入任务队列，但只有在独立探针确认 Unity 主线程后，命中当前 tick 才会执行队列；控制台线程不再因首次触发 `Drain()` 而被登记为主线程。
- Scheduler 在会话初始化或首次安装 tick 时按顺序尝试 `UnityEngine.UnitySynchronizationContext.ExecuteTasks` 和 `UnityEngine.Time.get_deltaTime` 作为一次性主线程探针。前者对应 Unity PlayerLoop 的主线程入口；不可用时才回退到 `get_deltaTime`。
- 探针只接受非嵌套原生分发，并使用原子 CAS 绑定首个有效线程。控制台 `runtime_invoke`、Lua 回调引发的嵌套 Hook 和其他嵌套分发不会注册主线程；主线程尚未确认前，队列保持等待。
- `il2cpp.set_tick(method)` 只切换调度入口，不再清空或重新绑定已确认的主线程 ID。切换到普通实例方法后，控制台先调用该方法也不会因此执行已排队任务。
- 主线程探针安装失败时，`set_tick()` 返回 `false, "failed to install main-thread probe or tick hook"`；已排队任务保留，后续再次设置 tick 可以重试安装。`is_tick_ready()` 仍只表示调度 tick Hook 已安装，不表示主线程探针已经收到有效事件。
- tick 队列的既有边界保持不变：任务仍按有界队列保存，队列满或 Lua 引用分配失败时 `schedule()` 失败；探针或 tick 暂不可用时不会丢弃已成功入队的任务。
- 相关实现见 [`src/il2cpp_scheduler.cpp`](../src/il2cpp_scheduler.cpp)、[`src/il2cpp_scheduler.h`](../src/il2cpp_scheduler.h) 和 [`README.md`](../README.md) 的“Unity 主线程、调度与 Hook”说明。

<a name="hooks"></a>
## 3. 内部探针与 Hook 共存

- 主线程探针复用现有原生 Hook 跳板和条目管理，但以独立的 `isMainThreadProbe` 状态与调度 tick 区分；同一 Method 仍可以同时承载用户 Hook 和内部 tick。
- 探针确认线程后立即停用探针身份；如果该 Method 没有同时作为 tick 或持有用户回调，底层探针 Hook 也会被禁用。若与 tick、用户 Hook 或待切换状态共存，则保留所需的底层 Hook。
- `Method:unhook()` 和 `il2cpp.unhook_all()` 不会误删尚未完成的主线程探针；tick 切换也不会因清理旧入口而破坏探针。正常关闭时才重置探针和线程身份。
- 相关实现见 [`src/il2cpp_hook.cpp`](../src/il2cpp_hook.cpp) 和 [`src/il2cpp_hook.h`](../src/il2cpp_hook.h)。本次不改变用户 Hook 的回调签名、`original()` 行为或 Hook 支持范围。

<a name="compatibility-and-limitations"></a>
## 4. 兼容性与限制

- 优先探针要求目标运行时能解析 `UnityEngine.UnitySynchronizationContext.ExecuteTasks` 并为其取得有效原生地址；裁剪、版本差异或导出/元数据不可用时会尝试 `Time.get_deltaTime`。
- `get_deltaTime` 只是回退入口，仍依赖游戏通常在 Unity 主线程调用它，无法像 `ExecuteTasks` 一样提供明确的 PlayerLoop 线程语义。如果两个探针都无法安装，不能安全确认主线程，`set_tick()` 会失败。
- 该机制只保证“已确认线程”才排空调度队列，不会验证游戏自定义入口是否始终只在主线程调用。选择自定义 tick 时仍应使用稳定且频繁、确实运行在 Unity 主线程的方法。
- 本次变更没有增加新的 Lua 入口；现有 `get_tick()`、`is_tick_ready()`、`schedule()` 和 `set_tick()` 的公开名称保持不变。

<a name="project-and-documentation"></a>
## 5. 工程与文档调整

- Il2CppLua 与 Lune 的当前使用手册更新为 `4.1.1` 握手版本，并保留主线程探针、tick 切换和失败条件的使用说明。
- 新增本文件作为 4.1.1 的长期更新记录；发布平台使用的简短说明保存在本机 `.release_local/ReleaseOutline_4.1.1.md`，不进入 Git。
- 后续运行验证应覆盖：切换普通实例方法后控制台先调用、首次探针前控制台主动调用探针、探针与 tick/用户 Hook 共存、`unhook_all()` 后等待探针，以及两种探针均不可用的失败路径。
