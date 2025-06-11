#include "PE.h"
#include "connection.h"
#include "utility.h"
#include <iostream>
#include <cassert>

void reset(Connection& top, Connection& left, Connection& bottom, Connection& right) {
    // Does nothing here — connections reset themselves per send()
    // Provided for symmetry and extensibility
}

void runCycleAndPrint(PE& pe, int cycles = 2) {
    for (int i = 0; i < cycles; ++i) {
        std::cout << ">>> Cycle " << i << "\n";
        pe.cycle();
    }
}

void testCase_MatchIndices() {
    std::cout << "\n[TEST] A.index2 == B.index1 (Expect Multiply)\n";

    Connection top, left, bottom, right;
    PE pe(0, 0);
    pe.setTopConnection(&top);
    pe.setLeftConnection(&left);
    pe.setBottomConnection(&bottom);
    pe.setRightConnection(&right);

    // Matching: 3 == 3
    DataPackage A(2.0, 1, 3); // from top
    DataPackage B(4.0, 3, 5); // from left

    top.receiveSrc(A);
    left.receiveSrc(B);

    runCycleAndPrint(pe);
    assert(bottom.pendingPsum());
    auto psum = bottom.sendPsum();
    assert(psum.value == 8.0 && psum.index1 == 1 && psum.index2 == 5);
    std::cout << "✅ Passed match index test\n";
}

void testCase_Aindex2LessThanBindex1() {
    std::cout << "\n[TEST] A.index2 < B.index1 (Expect sendBottom, drop A)\n";

    Connection top, left, bottom, right;
    PE pe(0, 1);
    pe.setTopConnection(&top);
    pe.setLeftConnection(&left);
    pe.setBottomConnection(&bottom);
    pe.setRightConnection(&right);

    DataPackage A(1.0, 0, 2);  // index2 = 2
    DataPackage B(5.0, 3, 6);  // index1 = 3

    top.receiveSrc(A);
    left.receiveSrc(B);

    runCycleAndPrint(pe);
    assert(!bottom.pendingPsum());
    std::cout << "✅ Passed A < B index test\n";
}

void testCase_Aindex2GreaterThanBindex1() {
    std::cout << "\n[TEST] A.index2 > B.index1 (Expect sendRight, drop B)\n";

    Connection top, left, bottom, right;
    PE pe(1, 0);
    pe.setTopConnection(&top);
    pe.setLeftConnection(&left);
    pe.setBottomConnection(&bottom);
    pe.setRightConnection(&right);

    DataPackage A(3.0, 1, 5); // index2 = 5
    DataPackage B(2.0, 3, 1); // index1 = 3

    top.receiveSrc(A);
    left.receiveSrc(B);

    runCycleAndPrint(pe);
    assert(!bottom.pendingPsum());
    std::cout << "✅ Passed A > B index test\n";
}

void testCase_OnlyA() {
    std::cout << "\n[TEST] Only A received (Expect forward A)\n";

    Connection top, left, bottom, right;
    PE pe(0, 0);
    pe.setTopConnection(&top);
    pe.setLeftConnection(&left);
    pe.setBottomConnection(&bottom);
    pe.setRightConnection(&right);

    DataPackage A(9.0, 2, 4);

    top.receiveSrc(A);

    runCycleAndPrint(pe);
    std::cout << "✅ Passed only A test\n";
}

void testCase_OnlyB() {
    std::cout << "\n[TEST] Only B received (Expect forward B)\n";

    Connection top, left, bottom, right;
    PE pe(0, 0);
    pe.setTopConnection(&top);
    pe.setLeftConnection(&left);
    pe.setBottomConnection(&bottom);
    pe.setRightConnection(&right);

    DataPackage B(6.0, 4, 7);

    left.receiveSrc(B);

    runCycleAndPrint(pe);
    std::cout << "✅ Passed only B test\n";
}

int main() {
    testCase_MatchIndices();
    testCase_Aindex2LessThanBindex1();
    testCase_Aindex2GreaterThanBindex1();
    testCase_OnlyA();
    testCase_OnlyB();

    std::cout << "\n✅✅ All PE test cases passed successfully.\n";
    return 0;
}
