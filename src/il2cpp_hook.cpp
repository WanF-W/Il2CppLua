// IL2CPP 原生 Hook：注册表与 Lua 分发。仅 Windows x64 标准 IL2CPP ABI。
// 原始调用经 NativeInvoke 的独立栈帧调用 MinHook trampoline，不改变全局 Hook 状态。
// 注册表条目保留到 Shutdown；锁顺序固定为 LuaEngine -> 注册表。
// 参数和返回值转换共用 lua_value，tick 选择与排队策略属于 Scheduler。
#include "il2cpp_hook.h"
#include "hook_stub.h"
#include "il2cpp_scheduler.h"
#include "il2cpp_resolver.h"
#include "lua_binding_internal.h"
#include "lua_engine.h"
#include "pipe_channel.h"
#include "../minhook_src/MinHook.h"

#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

// NativeHookContext - 与 hook_stub.asm 共享的内存布局
// 由 HookDetourEntry 在栈上构造，分发器只读/写这些字段
struct NativeHookContext
{
    uint64_t rcx;          // 0x00: 第一个整数参数
    uint64_t rdx;          // 0x08: 第二个整数参数
    uint64_t r8;           // 0x10: 第三个整数参数
    uint64_t r9;           // 0x18: 第四个整数参数
    uint64_t xmm0;         // 0x20: XMM0 原始位（低 32 位为 float）
    uint64_t xmm1;         // 0x28: XMM1 原始位
    uint64_t xmm2;         // 0x30: XMM2 原始位
    uint64_t xmm3;         // 0x38: XMM3 原始位
    uint64_t stackArgs;    // 0x40: 第一个栈参数的地址（入口 rsp + 40）
    uint64_t hookId;       // 0x48: thunk 写入的 hook 编号（RAX）
    uint64_t resultInt;    // 0x50: 整数/指针/小结构体返回值
    uint64_t resultFloat;  // 0x58: float/double 返回值（原始位）
    void*    original;     // 0x60: NativeInvoke 调用目标 / 分发器回退标志
    int32_t  reserved;     // 0x68: bit 0/1 = Lua/registry 锁，bit 2 = 已调用原方法
};

// 布局必须与 hook_stub.asm 中的偏移完全一致
static_assert(offsetof(NativeHookContext, rcx)         == 0x00, "NativeHookContext.rcx offset mismatch");
static_assert(offsetof(NativeHookContext, rdx)         == 0x08, "NativeHookContext.rdx offset mismatch");
static_assert(offsetof(NativeHookContext, r8)          == 0x10, "NativeHookContext.r8 offset mismatch");
static_assert(offsetof(NativeHookContext, r9)          == 0x18, "NativeHookContext.r9 offset mismatch");
static_assert(offsetof(NativeHookContext, xmm0)        == 0x20, "NativeHookContext.xmm0 offset mismatch");
static_assert(offsetof(NativeHookContext, xmm1)        == 0x28, "NativeHookContext.xmm1 offset mismatch");
static_assert(offsetof(NativeHookContext, xmm2)        == 0x30, "NativeHookContext.xmm2 offset mismatch");
static_assert(offsetof(NativeHookContext, xmm3)        == 0x38, "NativeHookContext.xmm3 offset mismatch");
static_assert(offsetof(NativeHookContext, stackArgs)   == 0x40, "NativeHookContext.stackArgs offset mismatch");
static_assert(offsetof(NativeHookContext, hookId)      == 0x48, "NativeHookContext.hookId offset mismatch");
static_assert(offsetof(NativeHookContext, resultInt)   == 0x50, "NativeHookContext.resultInt offset mismatch");
static_assert(offsetof(NativeHookContext, resultFloat) == 0x58, "NativeHookContext.resultFloat offset mismatch");
static_assert(offsetof(NativeHookContext, original)    == 0x60, "NativeHookContext.original offset mismatch");
static_assert(offsetof(NativeHookContext, reserved)     == 0x68, "NativeHookContext.reserved offset mismatch");
static_assert(sizeof(NativeHookContext) == 0x70, "NativeHookContext size mismatch");
// 由本文件定义 供 hook_stub.asm 调用的分发器
extern "C" void HookDispatch(NativeHookContext* ctx);
// 实际分发逻辑（HookDispatch 的 SEH 安全壳调用）
static void DispatchSafe(NativeHookContext* ctx, bool nested);
// 参数类型缓存
// 安装 Hook 时把每个参数的反射信息缓存下来
// 缓存原生布局；类型兼容性与转换仍由统一的 Resolver/值转换接口处理。
struct HookParam
{
    int32_t typeEnum   = 0;    // Il2CppTypeEnum
    const Il2CppType* type = nullptr; // 参数原始 Il2CppType（供 original() 显式传参编组）
    int32_t valueSize  = 0;    // 值类型大小（未知为 0）
    bool isValueType   = false; // 参数是值类型（含泛型值类型）
};
// Hook 条目
struct HookEntry
{
    const Il2CppMethod* method = nullptr; // MethodInfo 指针
    void* target    = nullptr;            // methodPointer（MinHook 目标）
    void* thunk     = nullptr;            // 分配的 detour thunk
    void* original  = nullptr;            // MinHook 生成的 trampoline
    uint32_t hookId = 0;                  // thunk 中写入的编号

    bool enabled = false;                 // 当前是否处于启用状态
    bool isMainThreadProbe = false;       // 独立的一次性主线程探针
    bool isInternalTick = false;          // 同时作为调度 tick，可与用户回调共存

    Il2CppClass* klass = nullptr;         // 声明类
    bool isStatic = false;                // 是否静态方法
    bool isValueTypeClass = false;        // 声明类是否为值类型（影响 this 解读）

    int32_t paramCount = 0;               // 参数个数
    std::vector<HookParam> params;        // 参数类型缓存

    int32_t returnEnum = Il2CppTypeEnum::TYPE_VOID; // 返回值类型
    bool hasReturn = false;               // 是否存在非 void 返回值
    int32_t returnSize = 0;               // 返回值大小（值类型时）
    bool largeReturn = false;             // 非 1/2/4/8 字节 struct 使用隐藏返回缓冲区
    const Il2CppType* returnType = nullptr; // original() 推送返回值用的原始返回类型

    int luaRef = LUA_REFNIL;              // Lua registry 中的回调引用
};

// original() 无参调用时保存原始参数槽位的上限（64 个 8 字节槽位）
constexpr uint32_t HOOK_MAX_ARGS = 64;
// OriginalCallState - 每次回调调用分配一份的 original 闭包状态
// 作为 Lua userdata 保存在闭包 upvalue 中
// 回调结束后 active 置 false 闭包被保存到全局后再次调用会报错
struct OriginalCallState
{
    bool active = false;
    NativeHookContext* ctx = nullptr;
};

// NativeInvoke 拥有独立参数栈帧，支持 64 个声明参数及 IL2CPP 隐藏槽位。
extern "C" void NativeInvoke(NativeHookContext* ctx, uint32_t stackCount);

// original 闭包的 Lua C 函数（upvalue: 1 = HookEntry*, 2 = OriginalCallState*）
static int OriginalInvoke(lua_State* L);
// 全局状态
// 条目永不删除（普通卸载只禁用）直到 Shutdown
// 因此分发器在 g_mutex 外持有裸指针也是安全的
static std::mutex g_mutex;
static std::vector<std::unique_ptr<HookEntry>> g_entries;
static std::map<const Il2CppMethod*, uint32_t> g_index;

static std::atomic<bool> g_shutdown{false};
static bool g_minhookInitialized = false;

// 该计数由 C++ 与汇编共同维护，覆盖从进入 detour 到原函数返回的完整
// 时间段。仅统计 Lua 回调不足以保护原方法执行和 MinHook 状态，因为原方法
// 可能在 C++ 分发器内部通过 trampoline 执行。
extern "C" std::atomic<int64_t> g_activeDetours{0};

// 内部 tick Hook 仍由 Hook 引擎持有，调度队列与入口元数据由 Il2CppScheduler 管理。
static bool g_tickInstalled = false;
static uint32_t g_tickHookId = UINT32_MAX;

// mov r11,trampoline-slot + mov eax,id + absolute entry jump + trampoline slot.
constexpr uint32_t HOOK_THUNK_SIZE = 37;

// 汇编入口只传递 hookId。这里仅获取条目保存的 trampoline 作为回退标志；
// 原方法实际由 InvokeOriginalFallback 在 C++ 中执行。
static HookEntry* LookupEntry(uint64_t hookId)
{
    // A fault may have abandoned an installation holding the registry mutex.
    // Existing game callers must escape that wait and bypass Lua instead.
    std::unique_lock<std::mutex> lock(g_mutex, std::defer_lock);
    while (!lock.try_lock())
    {
        if (LuaEngine::Instance().IsFaulted()) return nullptr;
        Sleep(1);
    }
    if (hookId >= g_entries.size()) return nullptr;
    return g_entries[static_cast<size_t>(hookId)].get();
}

// MS x64 ABI 规定只有 1/2/4/8 字节的结构体按值使用寄存器传递
// 其他大小的结构体一律通过指针传递（隐藏返回缓冲区同理）
static bool HookIsRegisterStruct(int32_t size)
{
    return size == 1 || size == 2 || size == 4 || size == 8;
}
// 内部工具：MinHook 初始化
static bool EnsureMinHookInitialized()
{
    if (g_minhookInitialized) return true;
    MH_STATUS status = MH_Initialize();
    // 已经初始化视为成功（防止多次调用）
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return false;
    g_minhookInitialized = true;
    return true;
}
// 内部工具：thunk 分配
// thunk 机器码:
//   49 BB <slot64>       mov r11, trampoline-slot
//   B8 <id32>            mov eax, imm32     ; 传递 hookId
//   FF 25 00000000       jmp qword ptr [rip+0]
//   <detour 地址 8 字节>  ; 绝对跳转到 HookDetourEntry
static void* AllocateThunk(uint32_t hookId, void* detour)
{
    uint8_t* mem = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, HOOK_THUNK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (mem == nullptr) return nullptr;

    // The trampoline slot is published after MH_CreateHook, before enabling.
    mem[0] = 0x49;
    mem[1] = 0xBB;
    void* trampolineSlot = mem + 29;
    memcpy(mem + 2, &trampolineSlot, sizeof(void*));
    memset(mem + 29, 0, sizeof(void*));
    mem[10] = 0xB8;
    memcpy(mem + 11, &hookId, sizeof(uint32_t));

    // jmp qword ptr [rip + 0]
    mem[15] = 0xFF;
    mem[16] = 0x25;
    memset(mem + 17, 0, 4);

    // 紧跟指令的绝对地址
    memcpy(mem + 21, &detour, sizeof(void*));

    FlushInstructionCache(GetCurrentProcess(), mem, HOOK_THUNK_SIZE);
    return mem;
}

static void PublishTrampoline(HookEntry* entry)
{
    memcpy(static_cast<uint8_t*>(entry->thunk) + 29, &entry->original, sizeof(void*));
    FlushInstructionCache(GetCurrentProcess(), entry->thunk, HOOK_THUNK_SIZE);
}

// 填充用户 Hook 与内部 tick 共用的方法元数据。
// 两条路径都必须知道 this、参数槽位和返回值 ABI；如果只给 tick 安装
// 一个“空条目”，静态 getter 的返回值就会被错误地当成 void 丢失。
static bool PopulateHookMetadata(
    HookEntry* entry, const Il2CppMethod* method, Il2CppClass* fallbackClass)
{
    if (entry == nullptr || method == nullptr) return false;

    auto& resolver = Il2CppResolver::Instance();
    entry->method = method;
    entry->klass = resolver.GetMethodClass(method);
    if (entry->klass == nullptr) entry->klass = fallbackClass;
    if (!resolver.CanInvokeMethod(method)) return false;
    entry->isStatic = resolver.IsStaticMethod(method);
    entry->isValueTypeClass = resolver.IsValueType(entry->klass)
        || resolver.IsEnum(entry->klass);

    entry->paramCount = resolver.GetMethodParamCount(method);
    if (entry->paramCount < 0
        || entry->paramCount > static_cast<int32_t>(HOOK_MAX_ARGS)) return false;
    entry->params.clear();
    entry->params.reserve(static_cast<size_t>(entry->paramCount));

    // 泛型共享代码可能采用额外 ABI；普通调用仍通过 runtime_invoke 支持闭合泛型。
    if (resolver.IsGenericMethod(method) || resolver.IsInflatedMethod(method)) return false;
    for (int i = 0; i < entry->paramCount; ++i)
    {
        HookParam p;
        p.type = resolver.GetMethodParamType(method, i);
        if (p.type == nullptr || resolver.IsByRef(p.type)) return false;
        p.typeEnum = LuaBridge_GetEffectiveTypeEnum(p.type);
        const size_t size = LuaBridge_GetValueStorageSize(p.type);
        if (size == 0) return false;
        p.isValueType = p.typeEnum == Il2CppTypeEnum::TYPE_VALUETYPE;
        p.valueSize = static_cast<int32_t>(size);
        entry->params.push_back(p);
    }

    const Il2CppType* returnType = resolver.GetMethodReturnType(method);
    if (returnType == nullptr || resolver.IsByRef(returnType)) return false;
    entry->returnType = returnType;
    entry->returnEnum = returnType
        ? LuaBridge_GetEffectiveTypeEnum(returnType) : Il2CppTypeEnum::TYPE_VOID;
    entry->hasReturn = entry->returnEnum != Il2CppTypeEnum::TYPE_VOID;
    if (entry->hasReturn && LuaBridge_GetValueStorageSize(returnType) == 0) return false;

    if (entry->returnEnum == Il2CppTypeEnum::TYPE_VALUETYPE)
    {
        entry->returnSize = static_cast<int32_t>(LuaBridge_GetValueStorageSize(returnType));
        entry->largeReturn = !HookIsRegisterStruct(entry->returnSize);
    }

    return true;
}
// 内部 tick Hook 后端
// 调度策略位于 il2cpp_scheduler.cpp；这里仅负责复用原生 Hook 跳板。
static bool TryInstallTickHook(const Il2CppMethod* method, Il2CppClass* klass, bool probe = false)
{
    auto& resolver = Il2CppResolver::Instance();
    if (!resolver.IsInitialized() || method == nullptr) return false;
    if (klass == nullptr) klass = resolver.GetMethodClass(method);
    if (klass == nullptr) return false;

    // tick 只需借用方法的每次调用时机，原始参数和返回值会由通用跳板原样保留。
    void* target = resolver.GetMethodPointer(method);
    if (target == nullptr) return false;
    if (!EnsureMinHookInitialized()) return false;

    // MinHook 对同一原生地址只能创建一个 Hook。重复设置 tick、切回曾用 tick，
    // 或把已有用户 Hook 的方法设为 tick 时，直接复用原条目和 trampoline。
    for (uint32_t i = 0; i < g_entries.size(); ++i)
    {
        HookEntry* existing = g_entries[i].get();
        if (existing == nullptr || existing->target != target) continue;
        if (existing->method != method) return false;

        const MH_STATUS status = MH_EnableHook(target);
        if (status != MH_OK && status != MH_ERROR_ENABLED) return false;

        if (probe) existing->isMainThreadProbe = true;
        else existing->isInternalTick = true;
        existing->enabled = true;
        if (!probe)
        {
            g_tickHookId = i;
            g_tickInstalled = true;
        }
        return true;
    }

    // 构建条目，复用与用户 Hook 相同的 thunk + HookDetourEntry 机制
    // 只接受 PopulateHookMetadata 明确支持的签名。
    auto entry = std::make_unique<HookEntry>();
    entry->hookId = static_cast<uint32_t>(g_entries.size());
    entry->target = target;
    if (!PopulateHookMetadata(entry.get(), method, klass)) return false;
    entry->isInternalTick = !probe;
    entry->isMainThreadProbe = probe;
    entry->luaRef = LUA_REFNIL;

    entry->thunk = AllocateThunk(entry->hookId, GetHookDetourAddress());
    if (entry->thunk == nullptr) return false;

    MH_STATUS status = MH_CreateHook(target, entry->thunk, &entry->original);
    if (status != MH_OK)
    {
        VirtualFree(entry->thunk, 0, MEM_RELEASE);
        return false;
    }

    PublishTrampoline(entry.get());
    status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        MH_RemoveHook(target);
        VirtualFree(entry->thunk, 0, MEM_RELEASE);
        return false;
    }

    entry->enabled = true;
    g_entries.push_back(std::move(entry));
    if (!probe)
    {
        g_tickHookId = static_cast<uint32_t>(g_entries.size() - 1);
        g_tickInstalled = true;
    }
    return true;
}
// 参数槽位游标（x64 寄存器/栈分配）
// MS x64 ABI 按参数位置分配寄存器（与 System V 不同）:
// ·第 0..3 个参数按位置使用 RCX/RDX/R8/R9（整数）或 XMM0-XMM3（浮点）
//  例如 f(int, double, int) -> RCX, XMM1, R8
// ·位置 4 之后统一从栈槽开始（每个 8 字节）
// ·整数与浮点共享位置编号 只按类型选择寄存器文件
struct ArgCursor
{
    int pos      = 0; // 下一个寄存器位置（0..3 整数/浮点共享）
    int stackIdx = 0; // 已使用的栈槽数量
};

// 读取一个整数槽位（整数/指针/小结构体/隐藏返回缓冲区）
static uint64_t ReadIntArg(const NativeHookContext* ctx, ArgCursor& cur)
{
    if (cur.pos < 4)
    {
        const uint64_t* regs = &ctx->rcx;
        return regs[cur.pos++];
    }
    const uint64_t* stack = reinterpret_cast<const uint64_t*>(ctx->stackArgs);
    return stack[cur.stackIdx++];
}

// 读取一个浮点槽位（float 只使用低 32 位）
static uint64_t ReadXmmArg(const NativeHookContext* ctx, ArgCursor& cur)
{
    if (cur.pos < 4)
    {
        const uint64_t* regs = &ctx->xmm0;
        return regs[cur.pos++];
    }
    const uint64_t* stack = reinterpret_cast<const uint64_t*>(ctx->stackArgs);
    return stack[cur.stackIdx++];
}
// 原方法调用保留 this、参数寄存器、栈槽和尾部 MethodInfo。
static uint32_t NativeStackCount(const HookEntry* e)
{
    const uint32_t slots = e->paramCount + (e->isStatic ? 0 : 1) + (e->largeReturn ? 1 : 0) + 1;
    return slots > 4 ? slots - 4 : 0; // 最后一槽为 MethodInfo。
}

static bool CallNativeSafe(NativeHookContext* call, uint32_t stackCount)
{
    __try { NativeInvoke(call, stackCount); }
    // A C++ exception from the original call can be handled while Lua frames
    // remain intact. Native faults and poisoned nested Hooks must escape.
    __except (GetExceptionCode() == 0xE06D7363 && !LuaEngine::Instance().IsFaulted()
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
    LuaEngine::Instance().RequireHealthy();
    return true;
}

static bool InvokeOriginalFallback(const HookEntry* e, NativeHookContext* ctx)
{
    if (e == nullptr || ctx == nullptr || e->original == nullptr) return false;
    if ((ctx->reserved & 4) != 0) return true; // 已调用，不能在错误路径重复副作用。
    ctx->reserved |= 4;
    NativeHookContext call = *ctx;
    call.original = e->original;
    const bool ok = CallNativeSafe(&call, NativeStackCount(e));
    ctx->resultInt = call.resultInt;
    ctx->resultFloat = call.resultFloat;
    if (!ok)
    {
        ctx->resultInt = 0;
        ctx->resultFloat = 0;
        if (e->largeReturn)
        {
            memset(reinterpret_cast<void*>(ctx->rcx), 0, e->returnSize);
            ctx->resultInt = ctx->rcx;
        }
        PipeChannel::Instance().SendLog("[hook] original method raised an exception; invocation was not repeated");
    }
    return ok;
}

static int OriginalInvoke(lua_State* L)
{
    auto* e = static_cast<HookEntry*>(lua_touserdata(L, lua_upvalueindex(1)));
    auto* st = static_cast<OriginalCallState*>(lua_touserdata(L, lua_upvalueindex(2)));
    if (st == nullptr || !st->active || e == nullptr || st->ctx == nullptr)
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "original can only be called inside the hook callback");
    const int argc = lua_gettop(L);
    if (!lua_checkstack(L, e->paramCount * 2 + 8)) return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "original: Lua stack capacity exceeded");
    if (argc != 0 && argc != e->paramCount)
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "original: expected %d arguments, got %d", e->paramCount, argc);

    NativeHookContext call = *st->ctx;
    call.original = e->original;
    uint64_t stack[HOOK_MAX_ARGS] = {};
    const uint32_t stackCount = NativeStackCount(e);
    if (stackCount != 0) memcpy(stack, reinterpret_cast<void*>(call.stackArgs), stackCount * sizeof(uint64_t));
    call.stackArgs = reinterpret_cast<uint64_t>(stack);

    // this 与尾部 MethodInfo 始终沿用原调用。替换参数只更新声明参数槽位。
    int slot = (e->largeReturn ? 1 : 0) + (e->isStatic ? 0 : 1);
    for (int i = 0; argc != 0 && i < e->paramCount; ++i, ++slot)
    {
        const HookParam& p = e->params[i];
        const size_t size = LuaBridge_GetValueStorageSize(p.type);
        void* storage = LuaBridge_NewBuffer(L, size);
        void* param = nullptr;
        if (!LuaBridge_MarshalArg(L, i + 1, p.type, storage, param, size))
            return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "original: failed to marshal argument %d", i + 1);
        uint64_t raw = 0;
        if (LuaBridge_IsRefType(p.type))
        {
            raw = reinterpret_cast<uint64_t>(param);
            if (p.typeEnum == Il2CppTypeEnum::TYPE_STRING && param != nullptr)
                LuaBridge_PushInstance(L, static_cast<Il2CppObject*>(param));
        }
        else if (p.isValueType && !HookIsRegisterStruct(p.valueSize))
        {
            // MS x64 间接传值参数必须指向调用方副本，不能让原方法改写装箱对象。
            memcpy(storage, param, size);
            raw = reinterpret_cast<uint64_t>(storage);
        }
        else memcpy(&raw, param, size);
        if (slot < 4)
        {
            if (p.typeEnum == Il2CppTypeEnum::TYPE_R4 || p.typeEnum == Il2CppTypeEnum::TYPE_R8)
                reinterpret_cast<uint64_t*>(&call.xmm0)[slot] = raw;
            else reinterpret_cast<uint64_t*>(&call.rcx)[slot] = raw;
        }
        else stack[slot - 4] = raw;
    }
    // 独立返回缓冲区保留最近一次成功结果；失败不能留下部分写入的数据。
    if (e->largeReturn) call.rcx = reinterpret_cast<uint64_t>(LuaBridge_NewBuffer(L, e->returnSize));
    const bool attemptedBefore = (st->ctx->reserved & 4) != 0;
    // 在进入原方法前标记，抛异常也不允许 fallback 再次执行。
    st->ctx->reserved |= 4;
    if (!CallNativeSafe(&call, stackCount))
    {
        if (!attemptedBefore && e->largeReturn)
        {
            memset(reinterpret_cast<void*>(st->ctx->rcx), 0, e->returnSize);
            st->ctx->resultInt = st->ctx->rcx;
        }
        return LuaEngine::RaiseError(L, protocol::ErrorCategory::Il2Cpp, "original: C++ exception from native invocation; invocation was not repeated");
    }
    if (e->largeReturn)
        memcpy(reinterpret_cast<void*>(st->ctx->rcx), reinterpret_cast<void*>(call.rcx), e->returnSize);
    st->ctx->resultInt = e->largeReturn ? st->ctx->rcx : call.resultInt;
    st->ctx->resultFloat = call.resultFloat;
    if (!e->hasReturn) return 0;
    void* value = e->largeReturn ? reinterpret_cast<void*>(call.rcx)
        : (e->returnEnum == Il2CppTypeEnum::TYPE_R4 || e->returnEnum == Il2CppTypeEnum::TYPE_R8)
        ? static_cast<void*>(&call.resultFloat) : static_cast<void*>(&call.resultInt);
    return LuaBridge_PushFieldValue(L, e->returnType, value);
}
// 原生参数 -> Lua 值
// 压入 this 参数（实例对象 / 值类型装箱 / 静态方法压声明类）
static void PushThisValue(lua_State* L, const HookEntry* e, uint64_t raw)
{
    auto& resolver = Il2CppResolver::Instance();

    // 静态方法与 frida-il2cpp-bridge 一致: this 为声明类。
    if (e->isStatic)
    {
        LuaBridge_PushClass(L, e->klass);
        return;
    }

    if (raw == 0)
    {
        // 空实例指针不是正常的 C# 调用，但回调层仍应能安全地看到 nil，
        // 而不是在解读对象头时再次触发访问违例。
        LuaBridge_PushInstance(L, nullptr);
        return;
    }

    if (e->isValueTypeClass)
    {
        // Lua 观察到的是值类型快照；original 始终使用原生 this，保留原方法写入。
        LuaBridge_PushInstance(L, resolver.Box(e->klass, reinterpret_cast<void*>(raw)));
        return;
    }

    // 引用类型实例直接包装 类从对象头读取（兼容子类）
    LuaBridge_PushInstance(L, reinterpret_cast<Il2CppObject*>(raw));
}

// 压入一个原生参数（已经按类型读取对应槽位）
static void PushNativeParam(lua_State* L, const HookParam& p, const NativeHookContext* ctx, ArgCursor& cur)
{
    uint64_t raw = (p.typeEnum == Il2CppTypeEnum::TYPE_R4 || p.typeEnum == Il2CppTypeEnum::TYPE_R8)
        ? ReadXmmArg(ctx, cur) : ReadIntArg(ctx, cur);
    void* value = p.isValueType && !HookIsRegisterStruct(p.valueSize)
        ? reinterpret_cast<void*>(raw) : static_cast<void*>(&raw);
    LuaBridge_PushFieldValue(L, p.type, value);
}

static bool MarshalLuaReturn(lua_State* L, const HookEntry* e, NativeHookContext* ctx, int idx)
{
    if (!e->hasReturn) return true;
    idx = lua_absindex(L, idx);
    const size_t size = LuaBridge_GetValueStorageSize(e->returnType);
    void* storage = LuaBridge_NewBuffer(L, size);
    void* value = nullptr;
    if (!LuaBridge_MarshalArg(L, idx, e->returnType, storage, value, size))
    {
        lua_pop(L, 1);
        return false;
    }
    if (e->largeReturn)
    {
        memcpy(reinterpret_cast<void*>(ctx->rcx), value, size);
        ctx->resultInt = ctx->rcx;
    }
    else if (LuaBridge_IsRefType(e->returnType)) ctx->resultInt = reinterpret_cast<uint64_t>(value);
    else if (e->returnEnum == Il2CppTypeEnum::TYPE_R4 || e->returnEnum == Il2CppTypeEnum::TYPE_R8)
        memcpy(&ctx->resultFloat, value, size);
    else memcpy(&ctx->resultInt, value, size);
    lua_pop(L, 1);
    return true;
}
// Lua 回调调用
// 调用前必须已持有 LuaEngine 互斥锁（可重入）
// 返回 false 表示尚未执行原方法，需要 fallback；已执行 original 的错误路径复用结果。
static bool InvokePinnedCallback(lua_State* L, const HookEntry* e, int pinRef, NativeHookContext* ctx)
{
    const int base = lua_gettop(L);
    if (!lua_checkstack(L, e->paramCount + 12)) return false;

    // 压入回调函数
    lua_rawgeti(L, LUA_REGISTRYINDEX, pinRef);

    // 参数游标从第一个整数槽位开始
    ArgCursor cur;

    // 大结构体返回值会占用第一个整数槽位（隐藏返回缓冲区）
    if (e->largeReturn)
    {
        ReadIntArg(ctx, cur);
    }

    // 压入 this（实例方法消费一个整数槽位，静态方法压声明类）
    uint64_t thisRaw = 0;
    if (!e->isStatic) thisRaw = ReadIntArg(ctx, cur);
    PushThisValue(L, e, thisRaw);

    // 创建 original 闭包: function(this, original, ...参数)
    // original 允许在回调体内调用原始实现（可传替换参数）
    OriginalCallState* st = static_cast<OriginalCallState*>(
        lua_newuserdata(L, sizeof(OriginalCallState)));
    memset(st, 0, sizeof(*st));
    st->active = true;
    st->ctx = ctx;
    // 固定在回调栈下面，用户清空 original 参数并 collectgarbage 也不会回收状态。
    lua_pushvalue(L, -1);
    lua_insert(L, base + 1);

    // 闭包 upvalue: 1 = HookEntry*, 2 = 本次调用的状态
    lua_pushlightuserdata(L, const_cast<HookEntry*>(e));
    lua_pushvalue(L, -2);
    lua_pushcclosure(L, OriginalInvoke, 2);
    lua_replace(L, -2);

    // 压入全部参数
    for (int32_t i = 0; i < e->paramCount; ++i)
    {
        PushNativeParam(L, e->params[static_cast<size_t>(i)], ctx, cur);
    }

    // 注意: MethodInfo 位于参数末尾 分发器不需要读取它
    // 参数游标到此即止 不会影响后续任何读取

    // 调用 Lua 回调: function(this, original, ...参数) -> 返回值
    int nargs = 2 + e->paramCount;
    int nresults = e->hasReturn ? 1 : 0;
    LuaEngine::OutputCapture outputCapture;
    LuaEngine::Instance().BeginOutputCapture(outputCapture);
    int status = lua_pcall(L, nargs, nresults, 0);
    LuaEngine::Instance().RequireHealthy();
    LuaEngine::Instance().EndOutputCapture(outputCapture);

    // 回调结束后 original 闭包失效
    // （闭包若被保存到全局 之后调用会得到明确报错而不是访问悬空指针）
    st->active = false;

    if (status != LUA_OK)
    {
        // Lua 回调出错：记录错误并请求 C++ 回退到原方法。
        const char* err = lua_tostring(L, -1);
        char buf[512];
        snprintf(buf, sizeof(buf), "[hook] callback error: %s", err ? err : "(non-string error)");
        PipeChannel::Instance().SendLog(buf);
        lua_settop(L, base);
        return (ctx->reserved & 4) != 0;
    }

    // 读取返回值
    if (e->hasReturn)
    {
        if (!MarshalLuaReturn(L, e, ctx, -1))
        {
            PipeChannel::Instance().SendLog(
                "[hook] callback returned a value incompatible with the hooked method");
            lua_settop(L, base);
            return (ctx->reserved & 4) != 0;
        }
        lua_pop(L, 1);
    }

    lua_settop(L, base);
    return true;
}
// 分发器（SEH 安全壳）
// HookDetourEntry 汇编跳板调用此函数
// __try/__except 捕获参数编组/回调过程中的访问违例
// 原生故障隔离 VM；嵌套故障继续展开，最外层仅在尚未调用原方法时由汇编透传。
extern "C" void HookDispatch(NativeHookContext* ctx)
{
    if (ctx == nullptr) return;
    ctx->reserved = 0;
    void** originalSlot = static_cast<void**>(ctx->original);
    const bool nested = LuaEngine::EnterExecution();
    __try
    {
        __try
        {
            ctx->original = *originalSlot;
            DispatchSafe(ctx, nested);
        }
        __except ((LuaEngine::Instance().Quarantine(GetExceptionCode()),
            nested ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER))
        {
            // No VM/registry access or retry of an original already entered.
            // Otherwise the assembly epilogue restores arguments and bypasses Lua.
            if ((ctx->reserved & 4) != 0) ctx->original = nullptr;
        }
    }
    __finally
    {
        if ((ctx->reserved & 0x2) != 0)
        {
            g_mutex.unlock();
            ctx->reserved &= ~0x2;
        }
        if ((ctx->reserved & 0x1) != 0)
        {
            LuaEngine::Instance().GetMutex().unlock();
            ctx->reserved &= ~0x1;
        }
        LuaEngine::LeaveExecution();
        if (ctx->original != nullptr) ctx->original = originalSlot;
        // Escaping SEH skips the assembly epilogue. The module is quarantined
        // before unwinding, so it cannot unload while this frame still exists.
        if (AbnormalTermination()) g_activeDetours.fetch_sub(1, std::memory_order_acq_rel);
    }
}

// 实际分发逻辑（不能在 __try 函数中出现需要展开的 C++ 对象）
// 正常原方法回退由 NativeInvoke 完成；隔离和内部类型查询由汇编透传。
static void DispatchSafe(NativeHookContext* ctx, bool nested)
{
    // 异常恢复标志（供 HookDispatch 的 __except 使用）。bit 0 表示 Lua
    // 锁，bit 1 表示 registry 锁；两把锁都使用显式路径管理。
    ctx->reserved = 0;

    auto& engine = LuaEngine::Instance();
    if (engine.IsFaulted() || Il2CppResolver::IsTypeQueryActive()) return;
    HookEntry* e = LookupEntry(ctx->hookId);
    if (engine.IsFaulted()) return;
    int pinRef = LUA_REFNIL;
    bool ok = false;
    bool isTick = false;

    if (g_shutdown.load(std::memory_order_acquire))
    {
        // Shutdown 只会在完整 detour 计数归零后释放条目；当前调用仍可在
        // 不触碰 Lua 的情况下执行原方法，避免卸载窗口返回错误的默认值。
        e = LookupEntry(ctx->hookId);
        if (engine.IsFaulted()) return;
        if (e != nullptr && ctx->original != nullptr)
            InvokeOriginalFallback(e, ctx);
        ctx->original = nullptr;
        return;
    }

    // 附加当前线程到 IL2CPP 运行时
    // 回调线程可能从未 attach 过 调用 IL2CPP API 前必须附加
    if (Il2CppResolver::Instance().AttachThread() == nullptr)
    {
        InvokeOriginalFallback(e, ctx);
        ctx->original = nullptr;
        return;
    }

    // 锁顺序固定: Lua 互斥锁 -> Hook 注册表
    // LuaEngine 使用可重入互斥锁 回调内再次触发 Hook 不会死锁
    // 手动加锁/解锁 保证 SEH 异常时可以在 __except 中恢复
    while (!engine.GetMutex().try_lock())
    {
        if (engine.IsFaulted()) return;
        Sleep(1);
    }
    ctx->reserved |= 0x1;
    lua_State* L = engine.GetState();
    if (L == nullptr || !engine.IsInitialized()) goto cleanup;

    while (!g_mutex.try_lock())
    {
        if (engine.IsFaulted()) goto cleanup;
        Sleep(1);
    }
    ctx->reserved |= 0x2;

    if (ctx->hookId >= g_entries.size()) goto cleanup;
    e = g_entries[ctx->hookId].get();
    if (e == nullptr) goto cleanup;

    // 记录正常分发使用的 trampoline；汇编透传使用 thunk 中的稳定槽位。
    ctx->original = e->original;

    // 关闭中/未启用 -> 由 cleanup 在 C++ 中回退原方法。
    if (g_shutdown.load(std::memory_order_acquire) || !e->enabled) goto cleanup;

    // tick 身份可以与用户 Hook 共存。没有用户回调时只排空调度队列；
    // 有回调时继续 pin 并执行回调，随后再排空队列。
    // 控制台调用及 Lua 回调引发的嵌套分发不能确认线程身份。
    // 在用户回调之前识别，并在注册表锁内停用探针身份。
    if (e->isMainThreadProbe && !nested)
    {
        Il2CppScheduler::ObserveMainThread();
        e->isMainThreadProbe = false;
        if (!e->isInternalTick && e->luaRef == LUA_REFNIL)
        {
            MH_DisableHook(e->target);
            e->enabled = false;
        }
    }
    isTick = e->isInternalTick;
    if (isTick && e->luaRef == LUA_REFNIL)
    {
        g_mutex.unlock();
        ctx->reserved &= ~0x2;
        goto unlocked;
    }

    if (e->luaRef == LUA_REFNIL) goto cleanup;

    // 在锁内 pin 一份回调引用，防止回调执行期间被 Lua 层 unhook 释放。
    lua_rawgeti(L, LUA_REGISTRYINDEX, e->luaRef);
    if (!lua_isfunction(L, -1))
    {
        lua_pop(L, 1);
        goto cleanup;
    }
    pinRef = luaL_ref(L, LUA_REGISTRYINDEX);

    g_mutex.unlock();
    ctx->reserved &= ~0x2;

unlocked:
    if (pinRef != LUA_REFNIL)
    {
        // 执行 Lua 回调（期间持有 Lua 互斥锁）
        ok = InvokePinnedCallback(L, e, pinRef, ctx);
        if (ok) ctx->original = nullptr;
    }

    if (ctx->original != nullptr)
    {
        // 回调报错、返回值不兼容或仅有内部 tick 时，在当前 C++ 栈内
        // 调用原方法并把结果写回上下文。
        InvokeOriginalFallback(e, ctx);
        ctx->original = nullptr;
    }

    // Scheduler 负责确认当前线程并排空队列。用户回调与 tick 共存时，
    // 先完成当前方法回调，再执行排队的主线程任务。
    engine.RequireHealthy();
    if (isTick) Il2CppScheduler::Drain(L);
    engine.RequireHealthy();

    // 释放 pin 引用
    if (pinRef != LUA_REFNIL) luaL_unref(L, LUA_REGISTRYINDEX, pinRef);

cleanup:
    if ((ctx->reserved & 0x2) != 0)
    {
        g_mutex.unlock();
        ctx->reserved &= ~0x2;
    }
    if ((ctx->reserved & 0x1) != 0)
    {
        engine.GetMutex().unlock();
        ctx->reserved &= ~0x1;
    }

    if (engine.IsFaulted())
    {
        if ((ctx->reserved & 4) != 0) ctx->original = nullptr;
        return;
    }
    if (e != nullptr && ctx->original != nullptr)
    {
        InvokeOriginalFallback(e, ctx);
    }
    ctx->original = nullptr;
}
// 公共 API：安装 Hook
bool Il2CppHook::HookMethod(lua_State* L, const Il2CppMethod* method, Il2CppClass* klass, int callbackIdx)
{
    if (L == nullptr || method == nullptr
        || g_shutdown.load(std::memory_order_acquire)) return false;

    auto& resolver = Il2CppResolver::Instance();

    // 复制并保存回调函数引用
    lua_pushvalue(L, callbackIdx);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    std::lock_guard<std::mutex> lock(g_mutex);
    // 初始检查与获取注册表锁之间可能发生 Shutdown；必须在锁内再次
    // 确认，否则清理完成后本次调用仍可能重新初始化 MinHook。
    if (g_shutdown.load(std::memory_order_acquire))
    {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return false;
    }

    // 已存在条目 -> 替换回调并重新启用
    auto it = g_index.find(method);
    if (it != g_index.end())
    {
        HookEntry* e = g_entries[it->second].get();
        const MH_STATUS status = MH_EnableHook(e->target);
        if (status != MH_OK && status != MH_ERROR_ENABLED)
        {
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return false;
        }
        if (e->luaRef != LUA_REFNIL) luaL_unref(L, LUA_REGISTRYINDEX, e->luaRef);
        e->luaRef = ref;
        e->enabled = true;
        return true;
    }

    // 初始化 MinHook
    if (!EnsureMinHookInitialized())
    {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return false;
    }

    // 读取原生函数指针
    void* target = resolver.GetMethodPointer(method);
    if (target == nullptr)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return false;
    }

    // 调度器可能已经为同一个 Method 安装了内部 tick。此时不能再次向
    // MinHook 创建同一地址的 Hook，而应复用原条目；保留 isInternalTick
    // 才能让 unhook 只移除用户回调而不破坏调度器。
    for (uint32_t i = 0; i < g_entries.size(); ++i)
    {
        HookEntry* existing = g_entries[i].get();
        if (existing == nullptr || existing->target != target) continue;
        if (existing->method != method)
        {
            // 不同 MethodInfo 共享一个 methodPointer 时，参数元数据可能
            // 不同；单个 detour 无法同时安全表示两套签名，明确拒绝。
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return false;
        }

        const MH_STATUS enableStatus = MH_EnableHook(target);
        if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED)
        {
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return false;
        }
        if (existing->luaRef != LUA_REFNIL)
            luaL_unref(L, LUA_REGISTRYINDEX, existing->luaRef);
        existing->luaRef = ref;
        existing->enabled = true;
        g_index[method] = i;
        return true;
    }

    // 构建条目并填充元数据
    auto entry = std::make_unique<HookEntry>();
    entry->hookId = static_cast<uint32_t>(g_entries.size());
    entry->target = target;
    if (!PopulateHookMetadata(entry.get(), method, klass))
    {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return false;
    }

    // 分配 thunk 并创建 MinHook
    entry->thunk = AllocateThunk(entry->hookId, GetHookDetourAddress());
    if (entry->thunk == nullptr)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return false;
    }

    MH_STATUS status = MH_CreateHook(target, entry->thunk, &entry->original);
    if (status != MH_OK)
    {
        // 同一个 methodPointer 可能被多个 MethodInfo 共享 无法重复创建
        VirtualFree(entry->thunk, 0, MEM_RELEASE);
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return false;
    }

    PublishTrampoline(entry.get());
    status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        MH_RemoveHook(target);
        VirtualFree(entry->thunk, 0, MEM_RELEASE);
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return false;
    }

    entry->enabled = true;
    entry->luaRef = ref;
    g_entries.push_back(std::move(entry));
    g_index[method] = static_cast<uint32_t>(g_entries.size() - 1);
    return true;
}
// 公共 API：卸载 Hook
bool Il2CppHook::UnhookMethod(const Il2CppMethod* method)
{
    if (method == nullptr) return false;

    lua_State* L = LuaEngine::Instance().GetState();

    std::lock_guard<std::mutex> lock(g_mutex);

    auto it = g_index.find(method);
    if (it == g_index.end()) return false;

    HookEntry* e = g_entries[it->second].get();

    // tick 与用户回调共享条目时只移除 Lua 回调，底层 Hook 必须保持启用。
    // 普通用户 Hook 仍只禁用、不移除，以便安全复用 trampoline。
    if (!e->isInternalTick && !e->isMainThreadProbe)
    {
        if (e->target != nullptr) MH_DisableHook(e->target);
        e->enabled = false;
    }

    if (e->luaRef != LUA_REFNIL && L != nullptr)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, e->luaRef);
    }
    e->luaRef = LUA_REFNIL;
    return true;
}
// 公共 API：查询 / 批量卸载
bool Il2CppHook::IsHooked(const Il2CppMethod* method)
{
    if (method == nullptr) return false;

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_index.find(method);
    if (it == g_index.end()) return false;

    HookEntry* e = g_entries[it->second].get();
    return e->enabled && e->luaRef != LUA_REFNIL;
}

void Il2CppHook::UnhookAll()
{
    lua_State* L = LuaEngine::Instance().GetState();

    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& entry : g_entries)
    {
        // 共享 tick 的条目只移除用户回调；纯用户 Hook 同时禁用底层 Hook。
        if (!entry->isInternalTick && !entry->isMainThreadProbe)
        {
            if (entry->target != nullptr) MH_DisableHook(entry->target);
            entry->enabled = false;
        }
        if (entry->luaRef != LUA_REFNIL && L != nullptr)
        {
            luaL_unref(L, LUA_REGISTRYINDEX, entry->luaRef);
        }
        entry->luaRef = LUA_REFNIL;
    }
}
// Scheduler 使用的内部 tick Hook 后端
bool Il2CppHook::InstallSchedulerTick(const Il2CppMethod* method, Il2CppClass* klass)
{
    if (method == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_shutdown.load(std::memory_order_acquire)) return false;

    const uint32_t oldId = g_tickHookId;
    if (!TryInstallTickHook(method, klass)) return false;
    if (oldId != g_tickHookId && oldId < g_entries.size())
    {
        HookEntry* old = g_entries[oldId].get();
        old->isInternalTick = false;
        if (old->luaRef == LUA_REFNIL && !old->isMainThreadProbe)
        {
            old->enabled = false;
            MH_DisableHook(old->target);
        }
    }
    return true;
}

bool Il2CppHook::InstallMainThreadProbe(const Il2CppMethod* method, Il2CppClass* klass)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_shutdown.load(std::memory_order_acquire)) return false;
    return TryInstallTickHook(method, klass, true);
}

bool Il2CppHook::IsSchedulerTickInstalled()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_tickInstalled;
}
// 公共 API：关闭模块
void Il2CppHook::Shutdown()
{
    auto& engine = LuaEngine::Instance();
    if (engine.IsFaulted()) return;

    // 先置关闭标志 阻止新的回调进入 Lua
    g_shutdown = true;

    // 禁用全部 Hook（MinHook API 本身线程安全）
    {
        std::unique_lock<std::mutex> lock(g_mutex, std::defer_lock);
        while (!lock.try_lock())
        {
            if (engine.IsFaulted()) return;
            Sleep(1);
        }
        if (engine.IsFaulted()) return;
        for (auto& entry : g_entries)
        {
            if (entry->target != nullptr) MH_DisableHook(entry->target);
        }
    }

    // 等待完整 detour 全部结束。计数覆盖汇编入口、Lua 分发和原方法回退，
    // 因此条目与 MinHook 资源不会在仍有线程执行时被释放。
    std::unique_lock<std::mutex> lock(g_mutex, std::defer_lock);
    while (!lock.try_lock())
    {
        if (engine.IsFaulted()) return;
        Sleep(1);
    }
    while (g_activeDetours.load(std::memory_order_acquire) != 0)
    {
        lock.unlock();
        if (engine.IsFaulted()) return;
        Sleep(10);
        while (!lock.try_lock())
        {
            if (engine.IsFaulted()) return;
            Sleep(1);
        }
    }
    if (engine.IsFaulted()) return;
    lua_State* L = engine.GetState();

    // 释放 Lua 引用与 thunk 并清空条目
    for (auto& entry : g_entries)
    {
        if (entry->target != nullptr) MH_RemoveHook(entry->target);
        if (entry->luaRef != LUA_REFNIL && L != nullptr)
        {
            luaL_unref(L, LUA_REGISTRYINDEX, entry->luaRef);
        }
        if (entry->thunk != nullptr) VirtualFree(entry->thunk, 0, MEM_RELEASE);
    }
    g_entries.clear();
    g_index.clear();

    // Lua VM 仍存活，在释放 Hook 后清理尚未执行的调度引用。
    Il2CppScheduler::Shutdown(L);
    g_tickInstalled = false;
    g_tickHookId = UINT32_MAX;

    lock.unlock();

    if (g_minhookInitialized)
    {
        MH_Uninitialize();
        g_minhookInitialized = false;
    }
    g_shutdown = false;
}
