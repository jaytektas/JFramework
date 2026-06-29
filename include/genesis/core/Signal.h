#pragma once

#include <iostream>
#include <vector>
#include <functional>
#include <memory>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <concepts>
#include "muted_logging_mock.h"

namespace { inline constexpr auto& LogUIEvent = jf::Log::JSignal; }

inline namespace jf {

class JSlotTracker;

// RAII connection management — widgets inherit this to auto-disconnect on destroy.
// Must be used on the main thread only.
class JSlotTracker {
public:
    JSlotTracker() = default;
    virtual ~JSlotTracker() {
        disconnectAll();
    }

    JSlotTracker(const JSlotTracker&) = delete;
    JSlotTracker& operator=(const JSlotTracker&) = delete;

    JSlotTracker(JSlotTracker&& other) noexcept : m_connections(std::move(other.m_connections)) {}
    JSlotTracker& operator=(JSlotTracker&& other) noexcept {
        if (this != &other) {
            disconnectAll();
            m_connections = std::move(other.m_connections);
        }
        return *this;
    }

    void addConnection(std::function<void()> disconnectFn) {
        m_connections.push_back(disconnectFn);
    }

    void disconnectAll() {
        for (auto& disconnect : m_connections) {
            if (disconnect) disconnect();
        }
        m_connections.clear();
    }

private:
    std::vector<std::function<void()>> m_connections;
};

// ============================================================================
// JSignal<Args...> — type-safe, MOC-free signal/slot.
//
// Thread-safety:
//   connect()  — safe from any thread
//   emit()     — safe from any thread; copies the slot list under the mutex
//                then fires callbacks WITHOUT the mutex held, so callbacks can
//                safely call connect()/disconnect() without deadlocking
//   ~JSignal()  — safe; marks all slots disconnected under the mutex
//
// Slots are disconnected lazily: the disconnected flag is an atomic<bool> so
// a slot that fires immediately after its owner is destroyed is a no-op rather
// than a use-after-free.
// ============================================================================
template <typename... Args>
class JSignal {
public:
    using SlotType = std::function<void(Args...)>;

    JSignal() = default;
    ~JSignal() {
        std::lock_guard<std::mutex> lk(m_slotsMutex);
        for (auto& s : m_slots)
            if (s && s->disconnected) s->disconnected->store(true);
    }

    JSignal(const JSignal&) = delete;
    JSignal& operator=(const JSignal&) = delete;

    // Connect a member function — auto-disconnects when receiver is destroyed.
    template <typename T>
    requires std::derived_from<T, JSlotTracker>
    void connect(T* receiver, void (T::*memberFunc)(Args...)) {
        if (!receiver) {
            qCWarning(LogUIEvent) << "Attempted to connect signal to a null receiver pointer." << std::endl;
            return;
        }

        auto slotInfo = std::make_shared<JSlotInternal>(
            [receiver, memberFunc](Args... args) {
                (receiver->*memberFunc)(std::forward<Args>(args)...);
            }
        );

        {
            std::lock_guard<std::mutex> lk(m_slotsMutex);
            m_slots.push_back(slotInfo);
        }

        auto disconnectedFlag = slotInfo->disconnected;
        receiver->addConnection([this, disconnectedFlag]() {
            disconnectedFlag->store(true, std::memory_order_release);
            purgeDisconnected();
        });
    }

    // Connect a plain callable — returns a disconnect function.
    // Discard the return value to connect permanently; store it in a
    // JSlotTracker::addConnection() call to disconnect on destroy.
    std::function<void()> connect(SlotType func) {
        auto slotInfo = std::make_shared<JSlotInternal>(std::move(func));
        {
            std::lock_guard<std::mutex> lk(m_slotsMutex);
            m_slots.push_back(slotInfo);
        }
        auto flag = slotInfo->disconnected;
        return [flag]() { flag->store(true, std::memory_order_release); };
    }

    // Fire all live slots. Copies slot list under lock then releases before
    // calling any callback — safe to connect/disconnect from inside a slot.
    void emit(Args... args) const {
        std::vector<std::shared_ptr<JSlotInternal>> active;
        {
            std::lock_guard<std::mutex> lk(m_slotsMutex);
            active = m_slots;
        }
        for (const auto& s : active) {
            if (s && !s->disconnected->load(std::memory_order_acquire))
                s->callback(args...);
        }
    }

    void operator()(Args... args) const {
        emit(std::forward<Args>(args)...);
    }

    void purgeDisconnected() {
        std::lock_guard<std::mutex> lk(m_slotsMutex);
        m_slots.erase(
            std::remove_if(m_slots.begin(), m_slots.end(),
                [](const std::shared_ptr<JSlotInternal>& s) {
                    return !s || s->disconnected->load(std::memory_order_relaxed);
                }),
            m_slots.end()
        );
    }

private:
    struct JSlotInternal {
        SlotType                        callback;
        std::shared_ptr<std::atomic<bool>> disconnected;

        explicit JSlotInternal(SlotType cb)
            : callback(std::move(cb))
            , disconnected(std::make_shared<std::atomic<bool>>(false)) {}
    };

    mutable std::mutex                             m_slotsMutex;
    mutable std::vector<std::shared_ptr<JSlotInternal>> m_slots;
};

} // inline namespace jf
