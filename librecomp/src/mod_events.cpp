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

struct EventListener {
    size_t mod_index;
    recomp::mods::GenericFunction func;
};

// Listeners registered against each event index.
std::vector<std::vector<EventListener>> g_event_listeners;

void invoke_listener(const recomp::mods::GenericFunction& func, uint8_t* rdram, recomp_context* ctx) {
    std::visit(overloaded{
        [rdram, ctx](recomp_func_t* native) { native(rdram, ctx); },
    }, func);
}

} // namespace

extern "C" {
    // The engine's built-in events are always registered first, so their base
    // index is fixed at zero.
    uint32_t builtin_base_event_index = 0;
}

extern "C" void recomp_trigger_event(uint8_t* rdram, recomp_context* ctx, uint32_t event_index) {
    if (event_index >= g_event_listeners.size()) {
        printf("Event %u triggered, but only %zu events have been registered!\n",
               event_index, g_event_listeners.size());
        assert(false);
        ultramodern::error_handling::message_box("Encountered an error with loaded mods: event index out of bounds");
        ULTRAMODERN_QUICK_EXIT();
    }

    // Every listener observes the same context the event fired with, so snapshot
    // it once and restore it after each call.
    const recomp_context fired_context = *ctx;
    for (const EventListener& listener : g_event_listeners[event_index]) {
        invoke_listener(listener.func, rdram, ctx);
        *ctx = fired_context;
    }
}

void recomp::mods::setup_events(size_t num_events) {
    g_event_listeners.resize(num_events);
}

void recomp::mods::register_event_callback(size_t event_index, size_t mod_index, GenericFunction callback) {
    g_event_listeners[event_index].push_back(EventListener{ mod_index, callback });
}

void recomp::mods::finish_event_setup(const ModContext& context) {
    // Run each event's listeners in mod load order.
    for (std::vector<EventListener>& listeners : g_event_listeners) {
        std::sort(listeners.begin(), listeners.end(),
            [&context](const EventListener& a, const EventListener& b) {
                return context.get_mod_order_index(a.mod_index) < context.get_mod_order_index(b.mod_index);
            });
    }
}

void recomp::mods::reset_events() {
    g_event_listeners.clear();
}
