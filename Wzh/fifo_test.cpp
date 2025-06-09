#include <iostream>
#include "fifo.h"
#include "Utility.h"

void test_basic_operations() {
    std::cout << "\n[TEST] Basic FIFO operations\n";
    FIFO<DataPackage> fifo(2);

    DataPackage d1(10, 0, 0);
    DataPackage d2(20, 1, 1);

    fifo.push(d1);
    fifo.push(d2);

    std::cout << "Front: " << fifo.front() << "\n";
    fifo.pop();

    std::cout << "Front after pop: " << fifo.front() << "\n";
    fifo.pop();

    std::cout << "Is Empty: " << fifo.isEmpty() << "\n";
}

void test_overflow() {
    std::cout << "\n[TEST] FIFO overflow\n";
    FIFO<DataPackage> fifo(1);

    try {
        fifo.push(DataPackage(1, 0, 0));
        fifo.push(DataPackage(2, 1, 1)); // should throw
    } catch (const std::overflow_error& e) {
        std::cout << "Caught expected overflow: " << e.what() << "\n";
    }
}

void test_underflow() {
    std::cout << "\n[TEST] FIFO underflow\n";
    FIFO<DataPackage> fifo;

    try {
        fifo.pop(); // should throw
    } catch (const std::underflow_error& e) {
        std::cout << "Caught expected underflow: " << e.what() << "\n";
    }

    try {
        fifo.front(); // should also throw
    } catch (const std::underflow_error& e) {
        std::cout << "Caught expected front underflow: " << e.what() << "\n";
    }
}

int main() {
    test_basic_operations();
    test_overflow();
    test_underflow();

    std::cout << "\nAll FIFO tests completed.\n";
    return 0;
}
