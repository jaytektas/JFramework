# 06. Compile-Time Signal/Event System

To surpass the rigidity and overhead of Qt’s Meta-Object Compiler (MOC), Genesis uses a purely compile-time, type-safe signaling system built on C++20/23 concepts and templates.

## Core Objectives
1. **Zero Runtime Overhead:** Direct function calls for synchronous signals.
2. **Type Safety:** Compile-time verification of argument types.
3. **No MOC:** Pure standard C++, no pre-processing steps.
4. **Lifecycle Awareness:** Automatic disconnection when either the emitter or receiver is destroyed (RAII).
5. **AI Observable:** Every signal dispatch can be optionally mirrored to the AI Control Bus.

## Technical Design: `Signal<Args...>`

### 1. The Delegate Structure
Signals maintain a list of `Delegate` objects. A delegate is a type-erased wrapper around a callable (lambda, member function, or free function).

```cpp
template<typename... Args>
class Signal {
public:
    // Connect a member function
    template<auto Method, typename T>
    ConnectionID Connect(T* instance);

    // Connect a lambda/functor
    ConnectionID Connect(std::function<void(Args...)> callback);

    // Emit the signal
    void Emit(Args... args);

private:
    struct Slot {
        uint64_t id;
        std::function<void(Args...)> invoker;
        WeakPtr<void> context; // For lifecycle tracking
    };
    std::vector<Slot> slots;
};
```

### 2. Lifecycle Management (RAII)
Genesis uses a `Trackable` base class and a `Connection` handle.
- **`Trackable`**: Objects that receive signals inherit from `Trackable`. It maintains a list of active connections.
- **`Connection`**: An RAII object. When it goes out of scope or the parent `Trackable` is destroyed, it sends a "disconnect" message back to the `Signal`.

### 3. Asynchronous Thread Dispatch
Signals can be emitted across thread boundaries. 
- If a signal is connected with `ConnectionType::Async`, the `Emit` call doesn't execute the slot immediately.
- Instead, it packages the arguments into a `Task` and pushes it to the receiver thread's **Task Queue** (part of Layer 3: Application & Event Loop).

## AI Control Bus Integration
The `Signal` class is instrumented for the AI Control Bus:
```cpp
void Signal::Emit(Args... args) {
    if (AICB::IsActive()) {
        AICB::LogSignal(this->SemanticID(), args...);
    }
    // Standard dispatch...
}
```
This allows an AI agent to "listen" to every internal event in the toolkit (e.g., button clicks, layout changes, data updates) without being explicitly connected to every widget.

## Example Usage
```cpp
class MyWidget : public Control {
public:
    Signal<int> OnValueChanged;
};

// ... elsewhere ...
auto connection = myWidget.OnValueChanged.Connect([](int val) {
    printf("Value changed to %d\n", val);
});
```

## Comparison with Qt/GTK
| Feature | Qt (MOC) | GTK (C/GObject) | Genesis (C++20) |
| :--- | :--- | :--- | :--- |
| **Type Safety** | Runtime/Generated | Weak (void*) | Full Compile-time |
| **Speed** | Slow (String lookup/MOC) | Moderate (VTable) | Direct Call / Inlined |
| **Tooling** | Requires `moc` | Macros / Boilerplate | Standard C++ |
| **AI Native** | No | No | Yes (via AICB Mirroring) |
