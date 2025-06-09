#ifndef FIFO_H
#define FIFO_H

#include <queue>

template<typename T>
class FIFO {
public:
    FIFO(size_t capacity = 1);  // default: 1-stage register

    bool isFull() const;
    bool isEmpty() const;
    void push(const T& item);
    T front() const;
    void pop();

private:
    std::queue<T> buffer;
    size_t max_capacity;
};

#endif // FIFO_H
