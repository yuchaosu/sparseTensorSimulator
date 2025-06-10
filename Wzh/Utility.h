#ifndef UTILITY_H
#define UTILITY_H

#include <iostream>
#include <limits>
#include <cmath>
constexpr int INVALID_INT = std::numeric_limits<int>::min();
constexpr double INVALID_DOUBLE = std::numeric_limits<double>::quiet_NaN();

#include <limits>

struct DataPackage {
    double value;
    int index1;
    int index2;

    DataPackage() 
        : value(INVALID_DOUBLE), index1(INVALID_INT), index2(INVALID_INT) {}

    DataPackage(double v, int i1, int i2) 
        : value(v), index1(i1), index2(i2) {}

    friend std::ostream& operator<<(std::ostream& os, const DataPackage& dp) {
        os << "(" << dp.value << ", " << dp.index1 << ", " << dp.index2 << ")";
        return os;
    }

    bool isValid() const {
        return !std::isnan(value) && index1 != INVALID_INT && index2 != INVALID_INT;
    }
};


#endif // UTILITY_H
