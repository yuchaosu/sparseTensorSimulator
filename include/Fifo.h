#ifndef FIFO_H
#define FIFO_H

#include <queue>
#include <stdexcept>

template<typename T>
class FIFO {
public:
    FIFO(size_t capacity = 20000) : max_capacity(capacity) {}

    bool isFull() const { return buffer.size() >= max_capacity; }
    bool isEmpty() const { return buffer.empty(); }

    void push(const T& item) {
        // A silent drop here would corrupt results with no diagnostic (missing
        // operands => missing products). Fail loudly instead so an undersized
        // FIFO can never masquerade as a correct run. Raise max_capacity (or add
        // real backpressure) if this fires on a legitimate workload.
        if (isFull()) {
            throw std::overflow_error("FIFO overflow: capacity exceeded (results would be wrong)");
        }
        buffer.push(item);
    }

    T front() const {
        if (isEmpty()) throw std::underflow_error("FIFO is empty");
        return buffer.front();
    }

    void pop() {
        if (isEmpty()) throw std::underflow_error("FIFO is empty");
        buffer.pop();
    }

    size_t size() const {
        return buffer.size();
    }

private:
    std::queue<T> buffer;
    size_t max_capacity;
};

#endif // FIFO_H
