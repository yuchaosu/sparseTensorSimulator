#ifndef PE_H
#define PE_H

#include <iostream>
#include <optional>
#include "Utility.h"

class PE {
public:
    PE();

    void receiveTop(const DataPackage& a);
    void receiveLeft(const DataPackage& b);
    void receivePsum(const DataPackage& psum);
    void compute();

    DataPackage getOutput();
    void setNeighbors(PE* up, PE* down, PE* left, PE* right);
    void logState(int row, int col, int cycle) const;

    void forwardTop();
    void forwardLeft();
    void forwardPsum();

    void cycle();

private:
    DataPackage receivedA, receivedB, result, forwardA, forwardB, PsumOut;
    bool topValid, leftValid, psumValid;

    PE *up, *down, *left, *right;
};

#endif // PE_H
