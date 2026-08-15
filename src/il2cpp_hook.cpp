/**
 * ============================================================
 * il2cpp_hook.cpp - IL2CPP 方法 Hook 模块实现
 * ============================================================
 * 核心思路
 * 
 * ·每个被 Hook 的方法分配一段可执行 thunk（mov eax, id; jmp HookDetour）
 * ·所有方法共用 hook_stub.asm 中的 HookDetour 跳板
 * ·跳板保存全部易失寄存器后调用 C++ 分发器 HookDispatch
 * ·分发器按 hookId 找到 HookEntry 再调用 Lua 回调
 * ·回调出错或异常时回跳 MinHook 生成的原始 trampoline
 * ·回调内可通过 original() 调用原方法（参考 frida-il2cpp-bridge）
 *   （临时禁用 Hook 后经 il2cpp_runtime_invoke 合法执行 与 mth:call 同路径）
 *
 * x64 参数布局（MS x64 ABI + IL2CPP 生成代码）
 * 
 * ·实例方法: (this, 参数..., MethodInfo)
 * ·静态方法: (参数..., MethodInfo)
 * ·大结构体返回值(>8字节): 第一个整数参数槽位为返回缓冲区指针
 * ·整数/指针/小结构体(<=8字节): RCX/RDX/R8/R9 或栈
 * ·float/double: XMM0-XMM3 或栈
 *
 * 线程安全
 * 
 * ·Lua 状态机使用可重入互斥锁（回调内可再次触发 Hook）
 * ·锁顺序固定为 Lua -> HookRegistry 避免死锁
 * ·回调期间额外 pin 一份 Lua 函数引用 防止回调内卸载导致悬空
 * ============================================================
 */

#include "il2cpp_hook.h"
#include "il2cpp_resolver.h"
#include "lua_bridge.h"
#include "lua_engine.h"
#include "pipe_channel.h"
#include "../minhook_src/MinHook.h"

#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ============================================================
// NativeHookContext - 与 hook_stub.asm 共享的内存布局
// ============================================================
// 由 HookDetour 在栈上构造 分发器只读/写这些字段
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
    void*    original;     // 0x60: 原始 trampoline（非空时回跳）
    int32_t  hasResult;    // 0x68: 预留
    int32_t  reserved;     // 0x6c: 预留
};

// 布局必须与 hook_stub.asm 中的 EQU 完全一致
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
static_assert(offsetof(NativeHookContext, hasResult)   == 0x68, "NativeHookContext.hasResult offset mismatch");
static_assert(sizeof(NativeHookContext) == 0x70, "NativeHookContext size mismatch");
// 由 hook_stub.asm 定义 所有 Hook 共享的跳板入口
extern "C" void HookDetour(void);
// 由本文件定义 供 hook_stub.asm 调用的分发器
extern "C" void HookDispatch(NativeHookContext* ctx);
// 实际分发逻辑（HookDispatch 的 SEH 安全壳调用）
static void DispatchSafe(NativeHookContext* ctx);

// ============================================================
// 参数类型缓存
// ============================================================
// 安装 Hook 时把每个参数的反射信息缓存下来
// 回调线程只需读取缓存 不再依赖反射导出函数
struct HookParam
{
    int32_t typeEnum   = 0;    // Il2CppTypeEnum
    const Il2CppType* type = nullptr; // 参数原始 Il2CppType（供 original() 显式传参编组）
    Il2CppClass* klass = nullptr; // 参数对应的类（值类型/引用类型）
    int32_t valueSize  = 0;    // 值类型大小（未知为 0）
    bool isValueType   = false; // 参数是值类型（含泛型值类型）
    bool isByRef       = false; // ref / out 参数（槽位是指针）
};

// ============================================================
// Hook 条目
// ============================================================
struct HookEntry
{
    const Il2CppMethod* method = nullptr; // MethodInfo 指针
    void* target    = nullptr;            // methodPointer（MinHook 目标）
    void* thunk     = nullptr;            // 分配的 detour thunk
    void* original  = nullptr;            // MinHook 生成的 trampoline
    uint32_t hookId = 0;                  // thunk 中写入的编号

    bool enabled = false;                 // 当前是否处于启用状态

    Il2CppClass* klass = nullptr;         // 声明类
    bool isStatic = false;                // 是否静态方法
    bool isValueTypeClass = false;        // 声明类是否为值类型（影响 this 解读）

    int32_t paramCount = 0;               // 参数个数
    std::vector<HookParam> params;        // 参数类型缓存

    int32_t returnEnum = Il2CppTypeEnum::TYPE_VOID; // 返回值类型
    bool hasReturn = false;               // 是否 void
    Il2CppClass* returnClass = nullptr;   // 返回值类（值类型时）
    int32_t returnSize = 0;               // 返回值大小（值类型时）
    bool largeReturn = false;             // 返回值 >8 字节 -> 隐藏返回缓冲区
    const Il2CppType* returnType = nullptr; // original() 推送返回值用的原始返回类型

    int luaRef = LUA_REFNIL;              // Lua registry 中的回调引用
};

// original() 无参调用时保存原始参数槽位的上限（64 个 8 字节槽位）
constexpr uint32_t HOOK_MAX_ARGS = 64;

// ============================================================
// OriginalCallState - 每次回调调用分配一份的 original 闭包状态
// ============================================================
// 作为 Lua userdata 保存在闭包 upvalue 中
// 回调结束后 active 置 false 闭包被保存到全局后再次调用会报错
struct OriginalCallState
{
    bool active = false;              // 是否处于回调执行期间
    const HookEntry* e = nullptr;     // 所属 Hook 条目
    NativeHookContext* ctx = nullptr; // 当前调用的原生上下文（回调期间有效）

    uint64_t thisRaw = 0;             // 原始 this（实例方法: 对象或裸数据指针）
    // 原始参数槽位快照（与声明参数一一对应 供无参 original() 使用）
    uint64_t argSlots[HOOK_MAX_ARGS] = {};
    int32_t argCount = 0;             // 快照参数个数
};

// original 闭包的 Lua C 函数（upvalue: 1 = HookEntry*, 2 = OriginalCallState*）
static int OriginalInvoke(lua_State* L);

// ============================================================
// 全局状态
// ============================================================
// 条目永不删除（卸载只禁用）直到 Shutdown
// 因此分发器在 g_mutex 外持有裸指针也是安全的
static std::mutex g_mutex;
static std::vector<std::unique_ptr<HookEntry>> g_entries;
static std::map<const Il2CppMethod*, uint32_t> g_index;

static std::atomic<bool> g_shutdown{false};
static std::atomic<int32_t> g_activeCallbacks{0};
static bool g_minhookInitialized = false;

// thunk 大小（mov eax,imm32 + jmp [rip+0] + 8 字节地址，16 字节对齐）
constexpr uint32_t HOOK_THUNK_SIZE = 16;

// MS x64 ABI 规定只有 1/2/4/8 字节的结构体按值使用寄存器传递
// 其他大小的结构体一律通过指针传递（隐藏返回缓冲区同理）
static bool HookIsRegisterStruct(int32_t size)
{
    return size == 1 || size == 2 || size == 4 || size == 8;
}

// ============================================================
// 内部工具：MinHook 初始化
// ============================================================
static bool EnsureMinHookInitialized()
{
    if (g_minhookInitialized) return true;
    MH_STATUS status = MH_Initialize();
    // 已经初始化视为成功（防止多次调用）
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return false;
    g_minhookInitialized = true;
    return true;
}

// ============================================================
// 内部工具：thunk 分配
// ============================================================
// thunk 机器码:
//   B8 <id32>            mov eax, imm32     ; 传递 hookId
//   FF 25 00000000       jmp qword ptr [rip+0]
//   <detour 地址 8 字节>  ; 绝对跳转到 HookDetour
static void* AllocateThunk(uint32_t hookId, void* detour)
{
    uint8_t* mem = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, HOOK_THUNK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (mem == nullptr) return nullptr;

    // mov eax, imm32
    mem[0] = 0xB8;
    memcpy(mem + 1, &hookId, sizeof(uint32_t));

    // jmp qword ptr [rip + 0]
    mem[5] = 0xFF;
    mem[6] = 0x25;
    mem[7] = 0x00;
    mem[8] = 0x00;
    mem[9] = 0x00;
    mem[10] = 0x00;

    // 紧跟指令的绝对地址
    memcpy(mem + 11, &detour, sizeof(void*));

    FlushInstructionCache(GetCurrentProcess(), mem, HOOK_THUNK_SIZE);
    return mem;
}

// ============================================================
// 参数槽位游标（x64 寄存器/栈分配）
// ============================================================
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

// ============================================================
// original() 参数编组（原始槽位 / Lua 显式参数 -> runtime_invoke params）
// ============================================================
// 执行原方法统一走 il2cpp_runtime_invoke（与 mth:call 同一条已验证路径）
// 参考 frida-il2cpp-bridge tracer: revert -> nativeFunction -> replace
// 对应这里: 临时禁用 Hook -> runtime_invoke -> 恢复 Hook
// 由 IL2CPP 运行时按内部 ABI 合法执行原方法（含装箱/虚分发/异常处理）
// 不再自造汇编调用帧

// 快照原始参数槽位（构造 original 闭包时调用）
// 无参 original() 时按声明参数顺序原样透传给 runtime_invoke
static void SnapshotOriginalArgs(OriginalCallState* st, const NativeHookContext* ctx, const HookEntry* e)
{
    ArgCursor cur;

    // 大结构体返回值会占用第一个整数槽位（隐藏返回缓冲区）
    if (e->largeReturn) ReadIntArg(ctx, cur);

    // this 单独保存 参数游标从声明参数开始
    if (!e->isStatic) st->thisRaw = ReadIntArg(ctx, cur);

    st->argCount = e->paramCount;
    for (int32_t i = 0; i < e->paramCount; ++i)
    {
        const HookParam& p = e->params[static_cast<size_t>(i)];
        if (p.typeEnum == Il2CppTypeEnum::TYPE_R4 || p.typeEnum == Il2CppTypeEnum::TYPE_R8)
        {
            st->argSlots[static_cast<size_t>(i)] = ReadXmmArg(ctx, cur);
        }
        else
        {
            st->argSlots[static_cast<size_t>(i)] = ReadIntArg(ctx, cur);
        }
    }
}

// 无参 original(): 把原始槽位值转为 runtime_invoke 的 params[i]
// 引用类型 / ref/out / 大值类型: 槽位本身就是指针 直接透传
// 基本类型 / 寄存器值类型: 拷入 storage 后传地址
static bool OriginalSlotToParam(const HookParam& p, uint64_t raw, void* storage, void*& outParam)
{
    // ref/out: 槽位即 byref 指针 原样透传（原方法直接读写调用方变量）
    if (p.isByRef)
    {
        outParam = reinterpret_cast<void*>(raw);
        return true;
    }

    // 浮点参数: XMM 槽位低 4/8 字节即数值
    if (p.typeEnum == Il2CppTypeEnum::TYPE_R4 || p.typeEnum == Il2CppTypeEnum::TYPE_R8)
    {
        memcpy(storage, &raw, (p.typeEnum == Il2CppTypeEnum::TYPE_R4) ? 4 : 8);
        outParam = storage;
        return true;
    }

    // 值类型: <=8 字节时槽位即原始位 其余槽位是数据指针
    if (p.isValueType)
    {
        if (HookIsRegisterStruct(p.valueSize))
        {
            memcpy(storage, &raw, static_cast<size_t>(p.valueSize));
            outParam = storage;
        }
        else
        {
            outParam = reinterpret_cast<void*>(raw);
        }
        return true;
    }

    // 引用类型: 槽位即对象指针 直接透传
    switch (p.typeEnum)
    {
    case Il2CppTypeEnum::TYPE_STRING:
    case Il2CppTypeEnum::TYPE_CLASS:
    case Il2CppTypeEnum::TYPE_OBJECT:
    case Il2CppTypeEnum::TYPE_SZARRAY:
    case Il2CppTypeEnum::TYPE_ARRAY:
    case Il2CppTypeEnum::TYPE_GENERICINST:
        outParam = reinterpret_cast<void*>(raw);
        return true;
    default:
        break;
    }

    // 基本类型: 寄存器按 8 字节零扩展 拷贝后低字节即正确值
    memcpy(storage, &raw, sizeof(uint64_t));
    outParam = storage;
    return true;
}

// 显式传参时 ref/out 参数编组（与旧版 hook 行为一致 回调内修改不回写）
static bool MarshalLuaByref(lua_State* L, int idx, const HookParam& p, void* storage, void*& outParam)
{
    auto& resolver = Il2CppResolver::Instance();

    // 值类型 byref: params[i] 即数据指针 数值拷入 storage 后传 storage
    if (p.isValueType)
    {
        if (p.valueSize <= 8)
        {
            uint64_t bits = 0;
            if (lua_islightuserdata(L, idx))
            {
                memcpy(&bits, lua_touserdata(L, idx), static_cast<size_t>(p.valueSize));
            }
            else if (!lua_isnil(L, idx))
            {
                LuaInstanceUD* ud = LuaBridge_CheckInstance(L, idx);
                if (ud != nullptr && ud->obj != nullptr)
                {
                    void* data = resolver.Unbox(ud->obj);
                    if (data != nullptr) memcpy(&bits, data, static_cast<size_t>(p.valueSize));
                }
            }
            memcpy(storage, &bits, static_cast<size_t>(p.valueSize));
            outParam = storage;
        }
        else
        {
            // 大值类型 byref: 直接传数据指针
            if (lua_islightuserdata(L, idx)) outParam = lua_touserdata(L, idx);
            else if (!lua_isnil(L, idx))
            {
                LuaInstanceUD* ud = LuaBridge_CheckInstance(L, idx);
                outParam = (ud != nullptr && ud->obj != nullptr) ? resolver.Unbox(ud->obj) : nullptr;
            }
            else outParam = nullptr;
        }
        return true;
    }

    // 引用类型 byref: params[i] 指向保存对象指针的槽位
    Il2CppObject* obj = nullptr;
    if (!lua_isnil(L, idx))
    {
        LuaInstanceUD* ud = LuaBridge_CheckInstance(L, idx);
        if (ud != nullptr) obj = ud->obj;
    }
    *reinterpret_cast<Il2CppObject**>(storage) = obj;
    outParam = storage;
    return true;
}

// 调用原方法: 临时禁用 Hook 保证 runtime_invoke 不重入 结束后恢复
// 使用 __try/__finally 确保即使原生访问违例也会恢复 Hook
// （本函数没有需要展开的 C++ 对象 满足 /EHsc 的 C2712 限制）
static Il2CppObject* CallOriginalSafe(const Il2CppMethod* method, void* obj, void** params, void* hookTarget, Il2CppException** exc)
{
    // hookTarget 非空表示调用方已临时禁用 Hook 由本函数负责恢复
    Il2CppObject* result = nullptr;
    __try
    {
        result = Il2CppResolver::Instance().RuntimeInvoke(method, obj, params, exc);
    }
    __finally
    {
        // 无论正常返回还是访问违例都必须恢复 Hook
        if (hookTarget != nullptr)
        {
            MH_STATUS es = MH_EnableHook(hookTarget);
            if (es != MH_OK && es != MH_ERROR_ENABLED)
            {
                PipeChannel::Instance().SendLog("[hook] failed to re-enable hook after original()");
            }
        }
    }
    return result;
}

// ============================================================
// original 闭包: 在回调体内调用原方法
// ============================================================
// 用法: function(this, original, ...)
//   ·original()            -- 使用原始参数调用原方法
//   ·original(a, b)        -- 使用替换参数调用原方法
//   ·返回值就是原方法的返回值
static int OriginalInvoke(lua_State* L)
{
    HookEntry* e = static_cast<HookEntry*>(lua_touserdata(L, lua_upvalueindex(1)));
    OriginalCallState* st = static_cast<OriginalCallState*>(lua_touserdata(L, lua_upvalueindex(2)));

    // 闭包只能在回调执行期间调用
    if (st == nullptr || !st->active || e == nullptr || st->ctx == nullptr || e->method == nullptr)
    {
        return luaL_error(L, "original can only be called inside the hook callback");
    }

    int argc = lua_gettop(L);
    bool explicitArgs = argc > 0;

    if (explicitArgs && argc != e->paramCount)
    {
        return luaL_error(L, "original: expected %d arguments, got %d", e->paramCount, argc);
    }

    auto& resolver = Il2CppResolver::Instance();

    // ---- 准备 this（与 mth:call 一致: 引用类型传对象指针 静态传 nullptr）----
    void* obj = nullptr;
    if (!e->isStatic)
    {
        if (e->isValueTypeClass)
        {
            // 值类型方法: this 可能是盒装对象 也可能是裸数据指针（Unity 版本差异）
            // 通过对象头第一个字段是否等于声明类来区分
            Il2CppClass* headKlass = nullptr;
            if (st->thisRaw != 0) headKlass = *reinterpret_cast<Il2CppClass**>(st->thisRaw);
            if (st->thisRaw != 0 && headKlass == e->klass)
            {
                obj = reinterpret_cast<void*>(st->thisRaw);
            }
            else if (st->thisRaw != 0)
            {
                // 裸数据指针: runtime_invoke 需要盒装对象 先装箱
                obj = resolver.Box(e->klass, reinterpret_cast<void*>(st->thisRaw));
            }
            // thisRaw == 0: 保持 nullptr 与原调用一致（原方法自身会处理）
        }
        else
        {
            obj = reinterpret_cast<void*>(st->thisRaw);
        }
    }

    // ---- 编组参数为 runtime_invoke 的 void** params ----
    // 使用固定大小数组（栈上分配）避免 longjmp 绕过析构导致资源泄漏
    if (e->paramCount > static_cast<int32_t>(HOOK_MAX_ARGS))
    {
        return luaL_error(L, "original: too many parameters (%d)", e->paramCount);
    }
    alignas(16) uint8_t storages[HOOK_MAX_ARGS * 16] = {};
    void* params[HOOK_MAX_ARGS] = {};

    for (int32_t i = 0; i < e->paramCount; ++i)
    {
        void*& outParam = params[static_cast<size_t>(i)];
        void* storage = storages + static_cast<size_t>(i) * 16;

        if (explicitArgs)
        {
            // 显式传参: ref/out 走专用编组 其余复用 mth:call 的 MarshalArg
            const HookParam& hp = e->params[static_cast<size_t>(i)];
            if (hp.isByRef)
            {
                if (!MarshalLuaByref(L, i + 1, hp, storage, outParam))
                {
                    return luaL_error(L, "original: failed to marshal argument %d", i + 1);
                }
                continue;
            }

            // 使用 Hook 安装时缓存的参数类型 回调线程不再触碰反射 API
            if (hp.type == nullptr)
            {
                return luaL_error(L, "original: failed to get parameter %d type", i + 1);
            }
            if (!LuaBridge_MarshalArg(L, i + 1, hp.type, storage, outParam))
            {
                return luaL_error(L, "original: failed to marshal argument %d", i + 1);
            }
        }
        else
        {
            // 无参 original(): 使用原始参数
            if (!OriginalSlotToParam(e->params[static_cast<size_t>(i)], st->argSlots[static_cast<size_t>(i)], storage, outParam))
            {
                return luaL_error(L, "original: failed to marshal argument %d", i + 1);
            }
        }
    }

    // ---- 临时禁用 Hook（失败则报错 避免 runtime_invoke 重入回调）----
    // runtime_invoke 会经 methodPointer 进入函数体
    // 若 Hook 仍启用会重新进入本回调 形成无限递归
    void* hookTarget = nullptr;
    if (e->target != nullptr)
    {
        MH_STATUS ds = MH_DisableHook(e->target);
        if (ds == MH_OK)
        {
            hookTarget = e->target;
        }
        else if (ds != MH_ERROR_DISABLED)
        {
            return luaL_error(L, "original: failed to disable hook (status %d)", static_cast<int>(ds));
        }
        // MH_ERROR_DISABLED: 已被其他路径临时禁用 直接调用原方法即可
    }

    // 调用原方法（结束后恢复 Hook） 此时线程已由分发器附加到 IL2CPP 运行时
    // 与 mth:call 的执行环境一致 由运行时合法执行原方法
    Il2CppException* exc = nullptr;
    Il2CppObject* result = CallOriginalSafe(
        e->method, obj, e->paramCount > 0 ? params : nullptr, hookTarget, &exc);

    if (exc != nullptr)
    {
        // 原方法抛出 C# 异常: 记录并转为 Lua 错误（分发器会回跳原始函数）
        PipeChannel::Instance().SendLog("[hook] original method threw a C# exception");
        return luaL_error(L, "original: C# exception thrown by method");
    }

    // 返回值与 mth:call 一致 通过 LuaBridge_PushReturnValue 转换
    LuaBridge_PushReturnValue(L, result, e->returnType);
    return 1;
}

// ============================================================
// 原生参数 -> Lua 值
// ============================================================

// 压入 this 参数（实例对象 / 值类型装箱 / 静态方法压声明类）
static void PushThisValue(lua_State* L, const HookEntry* e, uint64_t raw)
{
    auto& resolver = Il2CppResolver::Instance();

    // 静态方法与 frida-il2cpp-bridge 一致: this 为声明类
    if (e->isStatic)
    {
        LuaBridge_PushClass(L, e->klass);
        return;
    }

    if (e->isValueTypeClass)
    {
        // 值类型方法在部分 Unity 版本中 this 是装箱对象
        // 在 2021.2+ 中则是裸数据指针 通过对象头 klass 判断
        Il2CppClass* headKlass = nullptr;
        headKlass = *reinterpret_cast<Il2CppClass**>(raw);
        if (headKlass == e->klass)
        {
            LuaBridge_PushInstance(L, reinterpret_cast<Il2CppObject*>(raw), e->klass);
        }
        else
        {
            Il2CppObject* boxed = resolver.Box(e->klass, reinterpret_cast<void*>(raw));
            LuaBridge_PushInstance(L, boxed, e->klass);
        }
        return;
    }

    // 引用类型实例直接包装 类从对象头读取（兼容子类）
    LuaBridge_PushInstance(L, reinterpret_cast<Il2CppObject*>(raw), nullptr);
}

// 压入一个原生参数（已经按类型读取对应槽位）
static void PushNativeParam(lua_State* L, const HookParam& p, const NativeHookContext* ctx, ArgCursor& cur)
{
    auto& resolver = Il2CppResolver::Instance();

    // ref / out 参数: 槽位本身是指针
    if (p.isByRef)
    {
        uint64_t rawPtr = ReadIntArg(ctx, cur);
        void* ptr = reinterpret_cast<void*>(rawPtr);
        if (p.isValueType)
        {
            // 值类型 byref: 指针指向原始数据 装箱后包装为 Instance
            Il2CppObject* boxed = (ptr != nullptr) ? resolver.Box(p.klass, ptr) : nullptr;
            LuaBridge_PushInstance(L, boxed, p.klass);
        }
        else
        {
            // 引用类型 byref: 指针指向对象引用
            Il2CppObject* obj = nullptr;
            if (ptr != nullptr) obj = *reinterpret_cast<Il2CppObject**>(ptr);
            LuaBridge_PushInstance(L, obj, p.klass);
        }
        return;
    }

    // 浮点参数走 XMM 槽位
    if (p.typeEnum == Il2CppTypeEnum::TYPE_R4 || p.typeEnum == Il2CppTypeEnum::TYPE_R8)
    {
        uint64_t raw = ReadXmmArg(ctx, cur);
        if (p.typeEnum == Il2CppTypeEnum::TYPE_R4)
        {
            float f = 0.0f;
            memcpy(&f, &raw, sizeof(float));
            lua_pushnumber(L, f);
        }
        else
        {
            double d = 0.0;
            memcpy(&d, &raw, sizeof(double));
            lua_pushnumber(L, d);
        }
        return;
    }

    // 其余参数走整数槽位
    uint64_t raw = ReadIntArg(ctx, cur);

    // 值类型（含泛型值类型）: <=8 字节时槽位就是原始值 >8 字节时槽位是指针
    if (p.isValueType)
    {
        if (HookIsRegisterStruct(p.valueSize))
        {
            uint8_t buf[8] = {};
            memcpy(buf, &raw, static_cast<size_t>(p.valueSize));
            Il2CppObject* boxed = resolver.Box(p.klass, buf);
            LuaBridge_PushInstance(L, boxed, p.klass);
        }
        else
        {
            Il2CppObject* boxed = resolver.Box(p.klass, reinterpret_cast<void*>(raw));
            LuaBridge_PushInstance(L, boxed, p.klass);
        }
        return;
    }

    switch (p.typeEnum)
    {
    case Il2CppTypeEnum::TYPE_BOOLEAN:
        lua_pushboolean(L, (raw & 1) != 0);
        break;
    case Il2CppTypeEnum::TYPE_CHAR:
        lua_pushinteger(L, static_cast<uint16_t>(raw));
        break;
    case Il2CppTypeEnum::TYPE_I1:
        lua_pushinteger(L, static_cast<int8_t>(raw));
        break;
    case Il2CppTypeEnum::TYPE_U1:
        lua_pushinteger(L, static_cast<uint8_t>(raw));
        break;
    case Il2CppTypeEnum::TYPE_I2:
        lua_pushinteger(L, static_cast<int16_t>(raw));
        break;
    case Il2CppTypeEnum::TYPE_U2:
        lua_pushinteger(L, static_cast<uint16_t>(raw));
        break;
    case Il2CppTypeEnum::TYPE_I4:
        lua_pushinteger(L, static_cast<int32_t>(raw));
        break;
    case Il2CppTypeEnum::TYPE_U4:
        lua_pushinteger(L, static_cast<uint32_t>(raw));
        break;
    case Il2CppTypeEnum::TYPE_I8:
        lua_pushinteger(L, static_cast<int64_t>(raw));
        break;
    case Il2CppTypeEnum::TYPE_U8:
        // lua_Integer 为有符号 64 位 超过 INT64_MAX 会回绕
        lua_pushinteger(L, static_cast<lua_Integer>(raw));
        break;
    case Il2CppTypeEnum::TYPE_I:
    case Il2CppTypeEnum::TYPE_U:
    case Il2CppTypeEnum::TYPE_ENUM:
        lua_pushinteger(L, static_cast<lua_Integer>(raw));
        break;
    case Il2CppTypeEnum::TYPE_STRING:
    {
        Il2CppString* str = reinterpret_cast<Il2CppString*>(raw);
        if (str != nullptr) LuaBridge_PushString(L, str);
        else lua_pushnil(L);
        break;
    }
    case Il2CppTypeEnum::TYPE_CLASS:
    case Il2CppTypeEnum::TYPE_OBJECT:
    case Il2CppTypeEnum::TYPE_SZARRAY:
    case Il2CppTypeEnum::TYPE_ARRAY:
    case Il2CppTypeEnum::TYPE_GENERICINST:
        LuaBridge_PushInstance(L, reinterpret_cast<Il2CppObject*>(raw), p.klass);
        break;
    case Il2CppTypeEnum::TYPE_PTR:
    case Il2CppTypeEnum::TYPE_FNPTR:
    case Il2CppTypeEnum::TYPE_TYPEDBYREF:
        if (raw != 0) lua_pushlightuserdata(L, reinterpret_cast<void*>(raw));
        else lua_pushnil(L);
        break;
    default:
        // 未知类型按原始整数透传 避免误判导致崩溃
        lua_pushinteger(L, static_cast<lua_Integer>(raw));
        break;
    }
}

// ============================================================
// Lua 返回值 -> 原生返回值
// ============================================================

// 整数/指针/引用/小结构体返回值的编组
static uint64_t MarshalReturnInt(lua_State* L, const HookEntry* e, NativeHookContext* ctx, int idx)
{
    auto& resolver = Il2CppResolver::Instance();

    switch (e->returnEnum)
    {
    case Il2CppTypeEnum::TYPE_BOOLEAN:
        return lua_toboolean(L, idx) ? 1 : 0;
    case Il2CppTypeEnum::TYPE_CHAR:
        return static_cast<uint16_t>(lua_tointeger(L, idx));
    case Il2CppTypeEnum::TYPE_I1:
        return static_cast<uint8_t>(lua_tointeger(L, idx));
    case Il2CppTypeEnum::TYPE_U1:
        return static_cast<uint8_t>(lua_tointeger(L, idx));
    case Il2CppTypeEnum::TYPE_I2:
        return static_cast<uint16_t>(lua_tointeger(L, idx));
    case Il2CppTypeEnum::TYPE_U2:
        return static_cast<uint16_t>(lua_tointeger(L, idx));
    case Il2CppTypeEnum::TYPE_I4:
        return static_cast<uint32_t>(lua_tointeger(L, idx));
    case Il2CppTypeEnum::TYPE_U4:
        return static_cast<uint32_t>(lua_tointeger(L, idx));
    case Il2CppTypeEnum::TYPE_I8:
    case Il2CppTypeEnum::TYPE_U8:
    case Il2CppTypeEnum::TYPE_ENUM:
        return static_cast<uint64_t>(lua_tointeger(L, idx));
    case Il2CppTypeEnum::TYPE_I:
    case Il2CppTypeEnum::TYPE_U:
        if (lua_islightuserdata(L, idx)) return reinterpret_cast<uint64_t>(lua_touserdata(L, idx));
        return static_cast<uint64_t>(lua_tointeger(L, idx));
    case Il2CppTypeEnum::TYPE_STRING:
    {
        if (lua_isnil(L, idx)) return 0;
        const char* str = lua_tostring(L, idx);
        return str ? reinterpret_cast<uint64_t>(resolver.StringNew(str)) : 0;
    }
    case Il2CppTypeEnum::TYPE_CLASS:
    case Il2CppTypeEnum::TYPE_OBJECT:
    case Il2CppTypeEnum::TYPE_SZARRAY:
    case Il2CppTypeEnum::TYPE_ARRAY:
    case Il2CppTypeEnum::TYPE_GENERICINST:
    {
        if (lua_isnil(L, idx)) return 0;
        LuaInstanceUD* ud = LuaBridge_CheckInstance(L, idx);
        return ud ? reinterpret_cast<uint64_t>(ud->obj) : 0;
    }
    case Il2CppTypeEnum::TYPE_PTR:
    case Il2CppTypeEnum::TYPE_FNPTR:
        if (lua_islightuserdata(L, idx)) return reinterpret_cast<uint64_t>(lua_touserdata(L, idx));
        return static_cast<uint64_t>(lua_tointeger(L, idx));
    case Il2CppTypeEnum::TYPE_VALUETYPE:
    {
        // 大结构体: 写回调用者提供的隐藏返回缓冲区 返回值指向缓冲区
        if (e->largeReturn)
        {
            void* buffer = reinterpret_cast<void*>(ctx->rcx);
            if (buffer == nullptr) return 0;

            if (lua_islightuserdata(L, idx))
            {
                memcpy(buffer, lua_touserdata(L, idx), static_cast<size_t>(e->returnSize));
            }
            else if (!lua_isnil(L, idx))
            {
                LuaInstanceUD* ud = LuaBridge_CheckInstance(L, idx);
                if (ud != nullptr && ud->obj != nullptr)
                {
                    void* data = resolver.Unbox(ud->obj);
                    if (data != nullptr) memcpy(buffer, data, static_cast<size_t>(e->returnSize));
                    else memset(buffer, 0, static_cast<size_t>(e->returnSize));
                }
                else
                {
                    memset(buffer, 0, static_cast<size_t>(e->returnSize));
                }
            }
            else
            {
                memset(buffer, 0, static_cast<size_t>(e->returnSize));
            }
            return reinterpret_cast<uint64_t>(buffer);
        }

        // 小结构体: 返回位复制到 RAX
        uint64_t bits = 0;
        if (!lua_isnil(L, idx))
        {
            if (lua_islightuserdata(L, idx))
            {
                const void* data = lua_touserdata(L, idx);
                memcpy(&bits, data, static_cast<size_t>(e->returnSize));
            }
            else
            {
                LuaInstanceUD* ud = LuaBridge_CheckInstance(L, idx);
                if (ud != nullptr && ud->obj != nullptr)
                {
                    void* data = resolver.Unbox(ud->obj);
                    if (data != nullptr) memcpy(&bits, data, static_cast<size_t>(e->returnSize));
                }
            }
        }
        return bits;
    }
    default:
        return static_cast<uint64_t>(lua_tointeger(L, idx));
    }
}

// 把 Lua 回调返回值写入 ctx
static void MarshalLuaReturn(lua_State* L, const HookEntry* e, NativeHookContext* ctx, int idx)
{
    if (!e->hasReturn) return;

    // 浮点返回值写 XMM0
    if (e->returnEnum == Il2CppTypeEnum::TYPE_R4 || e->returnEnum == Il2CppTypeEnum::TYPE_R8)
    {
        double value = lua_tonumber(L, idx);
        if (e->returnEnum == Il2CppTypeEnum::TYPE_R4)
        {
            float f = static_cast<float>(value);
            memcpy(&ctx->resultFloat, &f, sizeof(float));
        }
        else
        {
            memcpy(&ctx->resultFloat, &value, sizeof(double));
        }
        return;
    }

    ctx->resultInt = MarshalReturnInt(L, e, ctx, idx);
}

// ============================================================
// Lua 回调调用
// ============================================================
// 调用前必须已持有 LuaEngine 互斥锁（可重入）
// 返回 false 时调用方应回跳原始函数
static bool InvokePinnedCallback(lua_State* L, const HookEntry* e, int pinRef, NativeHookContext* ctx)
{
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
    st->e = e;
    st->ctx = ctx;
    // 快照原始参数槽位 供无参 original() 透传
    SnapshotOriginalArgs(st, ctx, e);

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
    int status = lua_pcall(L, nargs, nresults, 0);

    // 回调结束后 original 闭包失效
    // （闭包若被保存到全局 之后调用会得到明确报错而不是访问悬空指针）
    st->active = false;

    if (status != LUA_OK)
    {
        // Lua 回调出错: 记录错误并回跳原始函数
        const char* err = lua_tostring(L, -1);
        char buf[512];
        snprintf(buf, sizeof(buf), "[hook] callback error: %s", err ? err : "(non-string error)");
        PipeChannel::Instance().SendLog(buf);
        lua_pop(L, 1);
        return false;
    }

    // 读取返回值
    if (e->hasReturn)
    {
        MarshalLuaReturn(L, e, ctx, -1);
        lua_pop(L, 1);
    }

    return true;
}

// ============================================================
// 分发器（SEH 安全壳）
// ============================================================
// HookDetour 汇编跳板调用此函数
// __try/__except 捕获参数编组/回调过程中的访问违例
// 任何异常都保持 ctx->original 非空 由汇编跳板回跳原始函数
extern "C" void HookDispatch(NativeHookContext* ctx)
{
    __try
    {
        DispatchSafe(ctx);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // 恢复在途回调计数与 Lua 互斥锁（/EHsc 下 SEH 不会展开 RAII）
        // ctx->hasResult / ctx->reserved 由 DispatchSafe 在进入危险区前写入
        if (ctx->hasResult != 0)
        {
            g_activeCallbacks.fetch_sub(1);
            ctx->hasResult = 0;
        }
        if (ctx->reserved != 0)
        {
            LuaEngine::Instance().GetMutex().unlock();
            ctx->reserved = 0;
        }
        // ctx->original 已在 DispatchSafe 中预设为原始 trampoline
        // 保持非空即可安全回跳 不再做任何 Lua 操作
        PipeChannel::Instance().SendLog("[hook] access violation in hook dispatcher\n");
    }
}

// 实际分发逻辑（不能在 __try 函数中出现需要展开的 C++ 对象）
static void DispatchSafe(NativeHookContext* ctx)
{
    // 异常恢复标志（供 HookDispatch 的 __except 使用）
    ctx->hasResult = 0;
    ctx->reserved = 0;

    HookEntry* e = nullptr;
    int pinRef = LUA_REFNIL;
    bool ok = false;

    auto& engine = LuaEngine::Instance();
    lua_State* L = engine.GetState();
    if (L == nullptr) return;

    // 附加当前线程到 IL2CPP 运行时
    // 回调线程可能从未 attach 过 调用 IL2CPP API 前必须附加
    Il2CppResolver::Instance().AttachThread();

    // 锁顺序固定: Lua 互斥锁 -> Hook 注册表
    // LuaEngine 使用可重入互斥锁 回调内再次触发 Hook 不会死锁
    // 手动加锁/解锁 保证 SEH 异常时可以在 __except 中恢复
    engine.GetMutex().lock();
    ctx->reserved = 1;
    if (!engine.IsInitialized()) goto cleanup;

    {
        std::lock_guard<std::mutex> hookLock(g_mutex);

        if (ctx->hookId >= g_entries.size()) goto cleanup;
        e = g_entries[ctx->hookId].get();
        if (e == nullptr) goto cleanup;

        // 预设原始 trampoline 回调失败/异常时回跳
        ctx->original = e->original;

        // 关闭中/未启用/无回调 -> 直接回跳原始函数
        if (g_shutdown.load() || !e->enabled || e->luaRef == LUA_REFNIL) goto cleanup;

        // 在锁内 pin 一份回调引用
        // 防止回调执行期间被 Lua 层 unhook 释放导致悬空
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->luaRef);
        if (!lua_isfunction(L, -1))
        {
            lua_pop(L, 1);
            goto cleanup;
        }
        pinRef = luaL_ref(L, LUA_REGISTRYINDEX);

        // 计入在途回调 供 Shutdown 等待
        g_activeCallbacks.fetch_add(1);
        ctx->hasResult = 1;
    }

    // 执行 Lua 回调（期间持有 Lua 互斥锁）
    ok = InvokePinnedCallback(L, e, pinRef, ctx);
    if (ok) ctx->original = nullptr;

    // 释放 pin 引用
    if (pinRef != LUA_REFNIL) luaL_unref(L, LUA_REGISTRYINDEX, pinRef);
    g_activeCallbacks.fetch_sub(1);
    ctx->hasResult = 0;

cleanup:
    if (ctx->reserved != 0)
    {
        engine.GetMutex().unlock();
        ctx->reserved = 0;
    }
}

// ============================================================
// 公共 API：安装 Hook
// ============================================================
bool Il2CppHook::HookMethod(lua_State* L, const Il2CppMethod* method, Il2CppClass* klass, int callbackIdx)
{
    if (L == nullptr || method == nullptr) return false;

    auto& resolver = Il2CppResolver::Instance();

    // 复制并保存回调函数引用
    lua_pushvalue(L, callbackIdx);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    std::lock_guard<std::mutex> lock(g_mutex);

    // 已存在条目 -> 替换回调并重新启用
    auto it = g_index.find(method);
    if (it != g_index.end())
    {
        HookEntry* e = g_entries[it->second].get();
        if (e->luaRef != LUA_REFNIL) luaL_unref(L, LUA_REGISTRYINDEX, e->luaRef);
        e->luaRef = ref;
        e->enabled = true;
        if (e->target != nullptr)
        {
            MH_STATUS status = MH_EnableHook(e->target);
            if (status != MH_OK && status != MH_ERROR_ENABLED)
            {
                e->enabled = false;
                e->luaRef = LUA_REFNIL;
                luaL_unref(L, LUA_REGISTRYINDEX, ref);
                return false;
            }
        }
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

    // 构建条目并填充元数据
    auto entry = std::make_unique<HookEntry>();
    entry->hookId = static_cast<uint32_t>(g_entries.size());
    entry->target = target;
    entry->method = method;
    entry->klass = resolver.GetMethodClass(method);
    if (entry->klass == nullptr) entry->klass = klass;
    entry->isStatic = resolver.IsStaticMethod(method);

    // 声明类是否为值类型（决定 this 指针解读方式）
    const Il2CppType* classType = entry->klass ? resolver.GetClassType(entry->klass) : nullptr;
    if (classType != nullptr)
    {
        int32_t classEnum = resolver.GetTypeEnum(classType);
        entry->isValueTypeClass = (classEnum == Il2CppTypeEnum::TYPE_VALUETYPE
            || classEnum == Il2CppTypeEnum::TYPE_ENUM);
    }

    // 缓存参数类型
    entry->paramCount = resolver.GetMethodParamCount(method);
    if (entry->paramCount < 0) entry->paramCount = 0;
    entry->params.reserve(static_cast<size_t>(entry->paramCount));

    for (int32_t i = 0; i < entry->paramCount; ++i)
    {
        const Il2CppType* paramType = resolver.GetMethodParamType(method, i);
        if (paramType == nullptr)
        {
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return false;
        }

        HookParam p;
        p.typeEnum = resolver.GetTypeEnum(paramType);
        p.type = paramType;

        if (p.typeEnum == Il2CppTypeEnum::TYPE_BYREF)
        {
            // ref/out 参数: 槽位是指针 需要知道目标类型
            p.isByRef = true;
            Il2CppClass* targetClass = resolver.GetClassFromType(paramType);
            if (targetClass != nullptr)
            {
                const Il2CppType* targetType = resolver.GetClassType(targetClass);
                int32_t targetEnum = targetType ? resolver.GetTypeEnum(targetType) : 0;
                if (targetEnum == Il2CppTypeEnum::TYPE_VALUETYPE
                    || targetEnum == Il2CppTypeEnum::TYPE_ENUM)
                {
                    p.isValueType = true;
                    p.klass = targetClass;
                    uint32_t align = 0;
                    p.valueSize = resolver.ClassValueSize(targetClass, &align);
                    if (p.valueSize <= 0)
                    {
                        luaL_unref(L, LUA_REGISTRYINDEX, ref);
                        return false;
                    }
                }
                else
                {
                    p.klass = targetClass;
                }
            }
        }
        else if (p.typeEnum == Il2CppTypeEnum::TYPE_VALUETYPE
            || p.typeEnum == Il2CppTypeEnum::TYPE_GENERICINST)
        {
            // 值类型 / 泛型实例: 判断实际是值类型还是引用类型
            Il2CppClass* valueClass = resolver.GetClassFromType(paramType);
            const Il2CppType* valueType = valueClass ? resolver.GetClassType(valueClass) : nullptr;
            int32_t valueEnum = valueType ? resolver.GetTypeEnum(valueType) : 0;
            if (valueEnum == Il2CppTypeEnum::TYPE_VALUETYPE)
            {
                p.isValueType = true;
                p.klass = valueClass;
                uint32_t align = 0;
                p.valueSize = resolver.ClassValueSize(valueClass, &align);
                if (p.valueSize <= 0)
                {
                    luaL_unref(L, LUA_REGISTRYINDEX, ref);
                    return false;
                }
            }
            else
            {
                // 引用类型泛型（如 List<T>）按引用对象处理
                p.klass = valueClass;
            }
        }
        else if (p.typeEnum == Il2CppTypeEnum::TYPE_CLASS
            || p.typeEnum == Il2CppTypeEnum::TYPE_OBJECT
            || p.typeEnum == Il2CppTypeEnum::TYPE_SZARRAY
            || p.typeEnum == Il2CppTypeEnum::TYPE_ARRAY)
        {
            p.klass = resolver.GetClassFromType(paramType);
        }

        entry->params.push_back(p);
    }

    // 缓存返回值信息
    const Il2CppType* retType = resolver.GetMethodReturnType(method);
    entry->returnType = retType;
    entry->returnEnum = retType ? resolver.GetTypeEnum(retType) : Il2CppTypeEnum::TYPE_VOID;
    entry->hasReturn = (entry->returnEnum != Il2CppTypeEnum::TYPE_VOID);

    // 泛型值类型返回值统一按值类型处理
    if (entry->returnEnum == Il2CppTypeEnum::TYPE_GENERICINST)
    {
        Il2CppClass* retClass = resolver.GetClassFromType(retType);
        const Il2CppType* retTypeInfo = retClass ? resolver.GetClassType(retClass) : nullptr;
        int32_t retEnum = retTypeInfo ? resolver.GetTypeEnum(retTypeInfo) : 0;
        if (retEnum == Il2CppTypeEnum::TYPE_VALUETYPE)
        {
            entry->returnEnum = Il2CppTypeEnum::TYPE_VALUETYPE;
            entry->returnClass = retClass;
            uint32_t align = 0;
            entry->returnSize = resolver.ClassValueSize(retClass, &align);
            if (entry->returnSize <= 0)
            {
                luaL_unref(L, LUA_REGISTRYINDEX, ref);
                return false;
            }
            entry->largeReturn = !HookIsRegisterStruct(entry->returnSize);
        }
    }
    else if (entry->returnEnum == Il2CppTypeEnum::TYPE_VALUETYPE)
    {
        entry->returnClass = resolver.GetClassFromType(retType);
        if (entry->returnClass == nullptr)
        {
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return false;
        }
        uint32_t align = 0;
        entry->returnSize = resolver.ClassValueSize(entry->returnClass, &align);
        if (entry->returnSize <= 0)
        {
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return false;
        }
        entry->largeReturn = !HookIsRegisterStruct(entry->returnSize);
    }

    // 分配 thunk 并创建 MinHook
    entry->thunk = AllocateThunk(entry->hookId, reinterpret_cast<void*>(&HookDetour));
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

// ============================================================
// 公共 API：卸载 Hook
// ============================================================
bool Il2CppHook::UnhookMethod(const Il2CppMethod* method)
{
    if (method == nullptr) return false;

    lua_State* L = LuaEngine::Instance().GetState();

    std::lock_guard<std::mutex> lock(g_mutex);

    auto it = g_index.find(method);
    if (it == g_index.end()) return false;

    HookEntry* e = g_entries[it->second].get();

    // 禁用 Hook（trampoline 保留 供再次安装复用）
    // 不调用 MH_RemoveHook 避免释放仍在途回调使用的 trampoline
    if (e->target != nullptr) MH_DisableHook(e->target);
    e->enabled = false;

    if (e->luaRef != LUA_REFNIL && L != nullptr)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, e->luaRef);
    }
    e->luaRef = LUA_REFNIL;
    return true;
}

// ============================================================
// 公共 API：查询 / 批量卸载
// ============================================================
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
        if (entry->target != nullptr) MH_DisableHook(entry->target);
        entry->enabled = false;
        if (entry->luaRef != LUA_REFNIL && L != nullptr)
        {
            luaL_unref(L, LUA_REGISTRYINDEX, entry->luaRef);
        }
        entry->luaRef = LUA_REFNIL;
    }
}

// ============================================================
// 公共 API：关闭模块
// ============================================================
void Il2CppHook::Shutdown()
{
    lua_State* L = LuaEngine::Instance().GetState();

    // 先置关闭标志 阻止新的回调进入 Lua
    g_shutdown = true;

    // 禁用全部 Hook（MinHook API 本身线程安全）
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& entry : g_entries)
        {
            if (entry->target != nullptr) MH_DisableHook(entry->target);
        }
    }

    // 等待在途回调全部结束（回调计数在 g_mutex 内增减）
    std::unique_lock<std::mutex> lock(g_mutex);
    while (g_activeCallbacks.load(std::memory_order_acquire) != 0)
    {
        lock.unlock();
        Sleep(10);
        lock.lock();
    }

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
    lock.unlock();

    if (g_minhookInitialized)
    {
        MH_Uninitialize();
        g_minhookInitialized = false;
    }
    g_shutdown = false;
}
