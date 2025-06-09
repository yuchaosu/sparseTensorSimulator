#include <iostream>
#include "PE.h"

void test_case(const DataPackage& top, const DataPackage& left) {
    PE pe;
    pe.load_inputs(top, left);
    pe.compute();

    std::cout << "Top input: " << top << std::endl;
    std::cout << "Left input: " << left << std::endl;

    std::cout << "Bottom output (psum): ";
    if (pe.bottom_out) std::cout << *pe.bottom_out << std::endl;
    else std::cout << "None" << std::endl;

    std::cout << "Forward Top: ";
    if (pe.forward_top) std::cout << *pe.forward_top << std::endl;
    else std::cout << "None" << std::endl;

    std::cout << "Forward Left: ";
    if (pe.forward_left) std::cout << *pe.forward_left << std::endl;
    else std::cout << "None" << std::endl;

    std::cout << "-----------------------------" << std::endl;
}

int main() {
    std::cout << "=== Mock Test for PE ===" << std::endl;

    // Case 1: Matching indices → multiplication
    test_case({5, 0, 2}, {3, 2, 1});

    // Case 2: top.index2 < left.index1 → forward top
    test_case({5, 0, 1}, {3, 2, 1});

    // Case 3: top.index2 > left.index1 → forward left
    test_case({5, 0, 3}, {3, 2, 1});

    return 0;
}
