#pragma once

#include <string>
#include <functional>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdint>
#include "Signal.h"
#include "BaseWidgets.h"
#include "SceneGraph.h"
#include "AiBusHook.h"

namespace Genesis {

// ============================================================================
// JDataBus — thread-safe reactive publish/subscribe data bus.
//
// Publishers push named channel values; subscribers receive them immediately.
// publish() is safe to call from any thread (e.g. serial worker, timer thread).
// subscribe() / bind() / unsubscribe() are typically called from the main thread.
//
// Callbacks are fired WITHOUT the internal mutex held, so subscribers can safely
// call subscribe()/unsubscribe() from within a callback.
//
// JBind a live hardware signal to a widget member in one call:
//   dataBus.bind("motor.rpm", &dial->value, dial);
// ============================================================================
class JDataBus {
public:
    using SubId = uint32_t;

    static JDataBus& instance() {
        static JDataBus inst;
        return inst;
    }

    // ---- Publish — safe from any thread ------------------------------------

    void publish(const std::string& channel, double value) {
        SubList<double> subs;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_lastDouble[channel] = value;
            auto it = m_doubleSubs.find(channel);
            if (it != m_doubleSubs.end()) subs = it->second;
        }
        for (auto& [id, cb] : subs) cb(value);
        if (JAiBusHook::emit) {
            std::string v = std::to_string(value);
            JAiBusHook::emit(0, ("databus:" + channel).c_str(), v.c_str());
        }
    }

    void publish(const std::string& channel, const std::string& value) {
        SubList<std::string> subs;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_lastString[channel] = value;
            auto it = m_stringSubs.find(channel);
            if (it != m_stringSubs.end()) subs = it->second;
        }
        for (auto& [id, cb] : subs) cb(value);
        if (JAiBusHook::emit)
            JAiBusHook::emit(0, ("databus:" + channel).c_str(), value.c_str());
    }

    // ---- Subscribe ---------------------------------------------------------

    SubId subscribe(const std::string& channel, std::function<void(double)> cb) {
        SubId id = ++m_nextId;
        std::lock_guard<std::mutex> lk(m_mutex);
        m_doubleSubs[channel].push_back({id, std::move(cb)});
        return id;
    }

    SubId subscribe(const std::string& channel, std::function<void(const std::string&)> cb) {
        SubId id = ++m_nextId;
        std::lock_guard<std::mutex> lk(m_mutex);
        m_stringSubs[channel].push_back({id, std::move(cb)});
        return id;
    }

    // ---- JBind — write member + invalidate widget automatically -------------

    SubId bind(const std::string& channel, double* member, JWidget* widget) {
        return subscribe(channel, [member, widget](double v) {
            *member = v;
            widget->invalidate();
        });
    }

    SubId bind(const std::string& channel, float* member, JWidget* widget) {
        return subscribe(channel, [member, widget](double v) {
            *member = static_cast<float>(v);
            widget->invalidate();
        });
    }

    SubId bind(const std::string& channel, int* member, JWidget* widget) {
        return subscribe(channel, [member, widget](double v) {
            *member = static_cast<int>(v);
            widget->invalidate();
        });
    }

    SubId bind(const std::string& channel, std::string* member, JWidget* widget) {
        return subscribe(channel, [member, widget](const std::string& v) {
            *member = v;
            widget->invalidate();
        });
    }

    void unsubscribe(const std::string& channel, SubId id) {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto removeFrom = [id](auto& map, const std::string& ch) {
            auto it = map.find(ch);
            if (it == map.end()) return;
            auto& vec = it->second;
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                      [id](const auto& p){ return p.first == id; }), vec.end());
        };
        removeFrom(m_doubleSubs, channel);
        removeFrom(m_stringSubs, channel);
    }

    void clear() {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_doubleSubs.clear();
        m_stringSubs.clear();
        m_lastDouble.clear();
        m_lastString.clear();
    }

    // ---- Last-value cache — safe from any thread ---------------------------

    double lastDouble(const std::string& ch, double def = 0.0) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_lastDouble.find(ch);
        return it != m_lastDouble.end() ? it->second : def;
    }

    std::string lastString(const std::string& ch, const std::string& def = {}) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_lastString.find(ch);
        return it != m_lastString.end() ? it->second : def;
    }

    // ---- Typed publish signals (for cross-component wiring) ----------------
    Core::JSignal<std::string, double>      onDoublePublished;
    Core::JSignal<std::string, std::string> onStringPublished;

private:
    JDataBus() = default;

    template<typename T>
    using SubList = std::vector<std::pair<SubId, std::function<void(T)>>>;

    mutable std::mutex                                    m_mutex;
    std::atomic<SubId>                                    m_nextId{0};
    std::unordered_map<std::string, SubList<double>>      m_doubleSubs;
    std::unordered_map<std::string, SubList<std::string>> m_stringSubs;
    std::unordered_map<std::string, double>               m_lastDouble;
    std::unordered_map<std::string, std::string>          m_lastString;
};

} // namespace Genesis
