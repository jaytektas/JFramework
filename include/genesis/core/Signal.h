#pragma once

#include <iostream>
#include <vector>
#include <functional>
#include <memory>
#include <algorithm>
#include <concepts>
#include "muted_logging_mock.h"

namespace { inline constexpr auto& LogUIEvent = Genesis::Log::Signal; }

namespace Core {

class SlotTracker;

/**
 * @brief Base class for automatic RAII connection management.
 */
class SlotTracker {
public:
    SlotTracker() = default;
    virtual ~SlotTracker() {
        disconnectAll();
    }

    SlotTracker(const SlotTracker&) = delete;
    SlotTracker& operator=(const SlotTracker&) = delete;
    
    SlotTracker(SlotTracker&& other) noexcept : m_connections(std::move(other.m_connections)) {}
    SlotTracker& operator=(SlotTracker&& other) noexcept {
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
            if (disconnect) {
                disconnect();
            }
        }
        m_connections.clear();
    }

private:
    std::vector<std::function<void()>> m_connections;
};

/**
 * @brief Modern C++20 Type-Safe, MOC-free Signal implementation.
 */
template <typename... Args>
class Signal {
public:
    using SlotType = std::function<void(Args...)>;

    Signal() = default;
    ~Signal() {
        for (auto& slotInfo : m_slots) {
            if (slotInfo->disconnected) {
                *slotInfo->disconnected = true;
            }
        }
    }

    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    template <typename T>
    requires std::derived_from<T, SlotTracker>
    void connect(T* receiver, void (T::*memberFunc)(Args...)) {
        if (!receiver) {
            qCWarning(LogUIEvent) << "Attempted to connect signal to a null receiver pointer." << std::endl;
            return;
        }

        auto slotInfo = std::make_shared<SlotInternal>(
            [receiver, memberFunc](Args... args) {
                (receiver->*memberFunc)(std::forward<Args>(args)...);
            }
        );

        m_slots.push_back(slotInfo);
        
        uintptr_t slotId = reinterpret_cast<uintptr_t>(slotInfo.get());
        auto disconnectedFlag = slotInfo->disconnected;

        receiver->addConnection([this, slotId, disconnectedFlag]() {
            *disconnectedFlag = true;
            this->purgeDisconnected();
        });
    }

    void connect(SlotType func) {
        m_slots.push_back(std::make_shared<SlotInternal>(std::move(func)));
    }

    void emit(Args... args) const {
        auto activeSlots = m_slots;
        
        for (const auto& slotInfo : activeSlots) {
            if (slotInfo && !*(slotInfo->disconnected)) {
                slotInfo->callback(args...);
            }
        }
    }

    void operator()(Args... args) const {
        emit(std::forward<Args>(args)...);
    }

private:
    struct SlotInternal {
        SlotType callback;
        std::shared_ptr<bool> disconnected;

        explicit SlotInternal(SlotType cb) 
            : callback(std::move(cb)), disconnected(std::make_shared<bool>(false)) {}
    };

    void purgeDisconnected() {
        m_slots.erase(
            std::remove_if(m_slots.begin(), m_slots.end(),
                [](const std::shared_ptr<SlotInternal>& slot) {
                    return !slot || *(slot->disconnected);
                }), 
            m_slots.end()
        );
    }

    mutable std::vector<std::shared_ptr<SlotInternal>> m_slots;
};

} // namespace Core
