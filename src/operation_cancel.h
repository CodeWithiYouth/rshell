#pragma once

#include <functional>
#include <utility>

namespace rshell {

using CancelCheck = std::function<bool()>;

// Cancellation follows synchronous adapters on the calling thread. A queued
// operation must capture its own shared state, never this thread-local slot.
inline thread_local CancelCheck operationCancelCheck;

inline CancelCheck currentOperationCancellation() { return operationCancelCheck; }
inline bool operationCancelled() { return operationCancelCheck && operationCancelCheck(); }

class OperationCancelScope {
public:
    explicit OperationCancelScope(CancelCheck check)
        : previous_(std::move(operationCancelCheck)) {
        operationCancelCheck = [previous = previous_, check = std::move(check)]() {
            return (previous && previous()) || (check && check());
        };
    }
    ~OperationCancelScope() { operationCancelCheck = std::move(previous_); }
    OperationCancelScope(const OperationCancelScope&) = delete;
    OperationCancelScope& operator=(const OperationCancelScope&) = delete;

private:
    CancelCheck previous_;
};

// A cooperative transfer may temporarily run an unrelated queued operation on
// the same worker. That operation inherits connection shutdown, not the paused
// transfer's cancellation token. Restore the transfer's scope on every exit.
class OperationCancelOverride {
public:
    explicit OperationCancelOverride(CancelCheck check)
        : previous_(std::move(operationCancelCheck)) {
        operationCancelCheck = std::move(check);
    }
    ~OperationCancelOverride() { operationCancelCheck = std::move(previous_); }
    OperationCancelOverride(const OperationCancelOverride&) = delete;
    OperationCancelOverride& operator=(const OperationCancelOverride&) = delete;

private:
    CancelCheck previous_;
};

}  // namespace rshell
