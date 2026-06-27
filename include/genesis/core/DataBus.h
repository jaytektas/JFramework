#pragma once

#include <string>
#include <functional>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include "Signal.h"
#include "BaseWidgets.h"
#include "SceneGraph.h"
#include "AiBusHook.h"

namespace Genesis {

// Reactive publish/subscribe data bus.
// Publishers push named channel values; subscribers receive them immediately.
//
// Key use case — bind a live hardware signal to a widget member in one call:
//   dataBus.bind("motor.rpm", &dial->value, dial);
//
// The bind() overload writes the value and calls invalidateNode on the widget's
// graph node so the next frame redraws automatically.  No explicit update() needed.

class DataBus {
public:
    using SubId = uint32_t;

    static DataBus& instance() {
        static DataBus inst;
        return inst;
    }

    // --- Publish ---

    void publish(const std::string& channel, double value) {
        m_lastDouble[channel] = value;
        auto it = m_doubleSubs.find(channel);
        if (it != m_doubleSubs.end())
            for (auto& [id, cb] : it->second) cb(value);
        if (AiBusHook::emit) {
            std::string v = std::to_string(value);
            AiBusHook::emit(0, ("databus:" + channel).c_str(), v.c_str());
        }
    }

    void publish(const std::string& channel, const std::string& value) {
        m_lastString[channel] = value;
        auto it = m_stringSubs.find(channel);
        if (it != m_stringSubs.end())
            for (auto& [id, cb] : it->second) cb(value);
        if (AiBusHook::emit)
            AiBusHook::emit(0, ("databus:" + channel).c_str(), value.c_str());
    }

    // --- Subscribe (raw callbacks) ---

    SubId subscribe(const std::string& channel, std::function<void(double)> cb) {
        SubId id = ++m_nextId;
        m_doubleSubs[channel].push_back({id, std::move(cb)});
        return id;
    }

    SubId subscribe(const std::string& channel, std::function<void(const std::string&)> cb) {
        SubId id = ++m_nextId;
        m_stringSubs[channel].push_back({id, std::move(cb)});
        return id;
    }

    // --- Bind — write member + invalidate widget automatically ---

    SubId bind(const std::string& channel, double* member, Widget* widget) {
        return subscribe(channel, [member, widget](double v) {
            *member = v;
            widget->invalidate();
        });
    }

    SubId bind(const std::string& channel, float* member, Widget* widget) {
        return subscribe(channel, [member, widget](double v) {
            *member = static_cast<float>(v);
            widget->invalidate();
        });
    }

    SubId bind(const std::string& channel, int* member, Widget* widget) {
        return subscribe(channel, [member, widget](double v) {
            *member = static_cast<int>(v);
            widget->invalidate();
        });
    }

    SubId bind(const std::string& channel, std::string* member, Widget* widget) {
        return subscribe(channel, [member, widget](const std::string& v) {
            *member = v;
            widget->invalidate();
        });
    }

    void clear() {
        m_doubleSubs.clear();
        m_stringSubs.clear();
        m_lastDouble.clear();
        m_lastString.clear();
    }

    void unsubscribe(const std::string& channel, SubId id) {
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

    // --- Last-value cache ---

    double      lastDouble(const std::string& ch, double      def = 0.0)  const {
        auto it = m_lastDouble.find(ch); return it != m_lastDouble.end() ? it->second : def;
    }
    std::string lastString(const std::string& ch, std::string def = {})   const {
        auto it = m_lastString.find(ch); return it != m_lastString.end() ? it->second : def;
    }

    // --- Typed publish signal (for cross-component wiring) ---
    Core::Signal<std::string, double>      onDoublePublished;
    Core::Signal<std::string, std::string> onStringPublished;

private:
    DataBus() = default;

    template<typename T>
    using SubList = std::vector<std::pair<SubId, std::function<void(T)>>>;

    std::unordered_map<std::string, SubList<double>>      m_doubleSubs;
    std::unordered_map<std::string, SubList<std::string>> m_stringSubs;
    std::unordered_map<std::string, double>               m_lastDouble;
    std::unordered_map<std::string, std::string>          m_lastString;
    SubId m_nextId{0};
};

} // namespace Genesis
