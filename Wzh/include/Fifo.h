#ifndef FIFO_H
#define FIFO_H

#include <queue>
#include <stdexcept>

template<typename T>
class FIFO {
public:
    //FIFO(size_t capacity = 1) : max_capacity(capacity) {}
    FIFO() = default; // Default constructor, no capacity limit

    //bool isFull() const { return buffer.size() >= max_capacity; }
    bool isEmpty() const { return buffer.empty(); }

    void push(const T& item) {
        // if (isFull()) throw std::overflow_error("FIFO is full");
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

private:
    std::queue<T> buffer;
    //size_t max_capacity;
};

#endif // FIFO_H
