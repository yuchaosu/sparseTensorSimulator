#include "PE.h"


PE::PE() : topValid(false), leftValid(false), psumValid(false), up(nullptr), down(nullptr), left(nullptr), right(nullptr) {}

void PE::receiveTop(const DataPackage& a) {
    receivedA = a;
    topValid = true;
}

void PE::receiveLeft(const DataPackage& b) {
    receivedB = b;
    leftValid = true;
}

void PE::receivePsum(const DataPackage& psum) {
    PsumOut = psum;
    psumValid = true;
}

void PE::compute() {
    if (topValid && leftValid) {
        result = DataPackage(receivedA.value * receivedB.value, receivedA.index1, receivedB.index2);
        psumValid = true;
    }
}

DataPackage PE::getOutput() {
    return result;
}

void PE::setNeighbors(PE* u, PE* d, PE* l, PE* r) {
    up = u;
    down = d;
    left = l;
    right = r;
}

void PE::forwardTop() {
    if (topValid && down) {
        down->receiveTop(receivedA);
        topValid = false;
        down->topValid = true;
    }
}

void PE::forwardLeft() {
    if (leftValid && right) {
        right->receiveLeft(receivedB);
        leftValid = false;
        right->leftValid = true;
    }
}

void PE::forwardPsum() {
    if (psumValid && down) {
        down->receiveTop(result);
        psumValid = false;
        down->psumValid = true;
    }
}

void PE::cycle() {
    setInputA(up->getOutput());
}

void PE::logState(int row, int col, int cycle) const {
    std::cout << "Cycle " << cycle << " | PE(" << row << "," << col << "): "
              << "A=" << (hasA ? std::to_string(A.value) : "NA") << ", "
              << "B=" << (hasB ? std::to_string(B.value) : "NA") << ", "
              << "Out=" << (psumValid ? std::to_string(result.value) : "NA") << "\n";
}