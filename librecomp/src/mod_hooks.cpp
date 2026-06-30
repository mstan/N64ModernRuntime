#include <algorithm>
#include <vector>

#include "librecomp/mods.hpp"
#include "librecomp/overlays.hpp"
#include "ultramodern/error_handling.hpp"

// Standard "overload set from lambdas" helper for std::visit.
template <class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

namespace {

struct RegisteredHook {
    size_t mod_index;
    recomp::mods::GenericFunction func;
};

struct HookSlot {
    std::vector<RegisteredHook> hooks;
    bool is_return_hook = false;
};

// One entry per hook slot the recompiled game exposes.
std::vector<HookSlot> g_hook_slots;

// Saved guest contexts, one per in-flight run_hook. A hooked function may
// itself reach another hooked function, so the contexts form a stack rather
// than a single slot. The base element is the resting context.
thread_local std::vector<recomp_context> g_context_stack = { recomp_context{} };

void dispatch_hook(const recomp::mods::GenericFunction& func, uint8_t* rdram, recomp_context* ctx) {
    std::visit(overloaded{
        [rdram, ctx](recomp_func_t* native) { native(rdram, ctx); },
    }, func);
}

} // namespace

void recomp::mods::run_hook(uint8_t* rdram, recomp_context* ctx, size_t hook_slot_index) {
    if (hook_slot_index >= g_hook_slots.size()) {
        printf("Hook slot %zu triggered, but only %zu hook slots have been registered!\n",
               hook_slot_index, g_hook_slots.size());
        assert(false);
        ultramodern::error_handling::message_box("Encountered an error with loaded mods: hook slot out of bounds");
        ULTRAMODERN_QUICK_EXIT();
    }

    // Remember the caller's context so each hook starts from the same state,
    // and so the return-value accessors can read it back.
    g_context_stack.emplace_back(*ctx);

    for (const RegisteredHook& hook : g_hook_slots[hook_slot_index].hooks) {
        dispatch_hook(hook.func, rdram, ctx);
        // Reset to the saved state before the next hook runs.
        *ctx = g_context_stack.back();
    }

    g_context_stack.pop_back();
}

void recomp::mods::setup_hooks(size_t num_hook_slots) {
    g_hook_slots.resize(num_hook_slots);
}

void recomp::mods::set_hook_type(size_t hook_slot_index, bool is_return) {
    g_hook_slots[hook_slot_index].is_return_hook = is_return;
}

void recomp::mods::register_hook(size_t hook_slot_index, size_t mod_index, GenericFunction callback) {
    g_hook_slots[hook_slot_index].hooks.push_back(RegisteredHook{ mod_index, callback });
}

void recomp::mods::finish_hook_setup(const ModContext& context) {
    // Order each slot's hooks by mod load order. Return hooks fire in the
    // reverse order of normal hooks, so flip the comparison for those.
    for (HookSlot& slot : g_hook_slots) {
        const bool reverse = slot.is_return_hook;
        std::sort(slot.hooks.begin(), slot.hooks.end(),
            [&context, reverse](const RegisteredHook& a, const RegisteredHook& b) {
                const auto oa = context.get_mod_order_index(a.mod_index);
                const auto ob = context.get_mod_order_index(b.mod_index);
                return reverse ? (oa > ob) : (oa < ob);
            });
    }
}

void recomp::mods::reset_hooks() {
    g_hook_slots.clear();
}

// Return-value accessors. Each exposes the value the just-returned hooked
// function left in the result register(s) of the saved context, applying the
// integer/float conversion implied by the declared return type. Exported under
// fixed names the recompiled mod imports, so the names are an ABI contract.

// Single-GPR result narrowed by `cast` (then sign/zero-extended back into r2).
#define HOOK_RETURN_R2(name, cast)                                          \
    void name(uint8_t* rdram, recomp_context* ctx) {                        \
        ctx->r2 = (gpr)(cast)g_context_stack.back().r2;                     \
    }

HOOK_RETURN_R2(recomphook_get_return_s32, int32_t)
HOOK_RETURN_R2(recomphook_get_return_u32, int32_t)   // matches the s32 path
HOOK_RETURN_R2(recomphook_get_return_ptr, int32_t)   // matches the s32 path
HOOK_RETURN_R2(recomphook_get_return_s16, int16_t)
HOOK_RETURN_R2(recomphook_get_return_u16, uint16_t)
HOOK_RETURN_R2(recomphook_get_return_s8,  int8_t)
HOOK_RETURN_R2(recomphook_get_return_u8,  uint8_t)

#undef HOOK_RETURN_R2

// 64-bit result occupies the r2:r3 pair.
void recomphook_get_return_s64(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = (gpr)(int32_t)g_context_stack.back().r2;
    ctx->r3 = (gpr)(int32_t)g_context_stack.back().r3;
}

void recomphook_get_return_u64(uint8_t* rdram, recomp_context* ctx) {
    recomphook_get_return_s64(rdram, ctx);
}

void recomphook_get_return_float(uint8_t* rdram, recomp_context* ctx) {
    ctx->f0.fl = g_context_stack.back().f0.fl;
}

void recomphook_get_return_double(uint8_t* rdram, recomp_context* ctx) {
    ctx->f0.fl = (gpr)(uint8_t)g_context_stack.back().f0.fl;
    ctx->f1.fl = (gpr)(uint8_t)g_context_stack.back().f1.fl;
}

#define REGISTER_FUNC(name) recomp::overlays::register_base_export(#name, name)

void recomp::mods::register_hook_exports() {
    REGISTER_FUNC(recomphook_get_return_s32);
    REGISTER_FUNC(recomphook_get_return_u32);
    REGISTER_FUNC(recomphook_get_return_ptr);
    REGISTER_FUNC(recomphook_get_return_s16);
    REGISTER_FUNC(recomphook_get_return_u16);
    REGISTER_FUNC(recomphook_get_return_s8);
    REGISTER_FUNC(recomphook_get_return_u8);
    REGISTER_FUNC(recomphook_get_return_s64);
    REGISTER_FUNC(recomphook_get_return_u64);
    REGISTER_FUNC(recomphook_get_return_float);
    REGISTER_FUNC(recomphook_get_return_double);
}
