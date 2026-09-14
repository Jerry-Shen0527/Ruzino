#pragma once

// Minimal string-keyed broadcast event bus (signal/slot).
//
// Extracted from GUI's WindowEventSystem so non-GUI modules (Stage, tests,
// headless tools) can own their own bus instances; `Window::events()` and
// `Stage::events()` are independent instances of this class. Callbacks fire
// synchronously, in subscription order, on the emitting thread.
//
// Deliberate limitations (fine for the current single-threaded owners):
// - No unsubscribe. A subscriber capturing a raw `this` must not outlive
//   its owner's bus (Stage works this way: it owns both the bus and the
//   subscription for its whole lifetime).
// - Calling subscribe() from inside a callback of the SAME event mutates
//   the vector being iterated and is undefined behavior.

#include <any>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Header-only: no export macro needed. Guard the namespace macros the same
// way every module's api.h does, so this header stands alone.
#ifndef RUZINO_NAMESPACE_OPEN_SCOPE
#define RUZINO_NAMESPACE_OPEN_SCOPE  namespace Ruzino {
#define RUZINO_NAMESPACE_CLOSE_SCOPE }
#endif

RUZINO_NAMESPACE_OPEN_SCOPE

class EventBus {
   public:
    using EventCallback = std::function<void(const std::string& event_data)>;
    using EventCallbackAny = std::function<void(const std::any& event_data)>;

    void subscribe(const std::string& event_name, EventCallback callback)
    {
        subscribers_[event_name].push_back(std::move(callback));
    }

    void subscribe_any(const std::string& event_name, EventCallbackAny callback)
    {
        subscribers_any_[event_name].push_back(std::move(callback));
    }

    void emit(const std::string& event_name, const std::string& event_data = "")
    {
        auto it = subscribers_.find(event_name);
        if (it != subscribers_.end()) {
            for (auto& callback : it->second) {
                callback(event_data);
            }
        }
    }

    void emit_any(const std::string& event_name, const std::any& event_data)
    {
        auto it = subscribers_any_.find(event_name);
        if (it != subscribers_any_.end()) {
            for (auto& callback : it->second) {
                callback(event_data);
            }
        }
    }

   private:
    std::unordered_map<std::string, std::vector<EventCallback>> subscribers_;
    std::unordered_map<std::string, std::vector<EventCallbackAny>>
        subscribers_any_;
};

RUZINO_NAMESPACE_CLOSE_SCOPE
