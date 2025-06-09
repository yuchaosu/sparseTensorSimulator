// fifo.tpp (or just include below fifo.h)

#include "fifo.h"
#include <stdexcept>

template<typename T>
FIFO<T>::FIFO(size_t capacity) : max_capacity(capacity) {}

template<typename T>
bool FIFO<T>::isFull() const {
    return buffer.size() >= max_capacity;
}

template<typename T>
bool FIFO<T>::isEmpty() const {
    return buffer.empty();
}

template<typename T>
void FIFO<T>::push(const T& item) {
    if (isFull())
        throw std::overflow_error("FIFO is full");
    buffer.push(item);
}

template<typename T>
T FIFO<T>::front() const {
    if (isEmpty())
        throw std::underflow_error("FIFO is empty");
    return buffer.front();
}

template<typename T>
void FIFO<T>::pop() {
    if (isEmpty())
        throw std::underflow_error("FIFO is empty");
    buffer.pop();
}
