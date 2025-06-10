#ifndef PE_H
#define PE_H

#include <iostream>
#include <optional>
#include "Utility.h"
#include "Fifo.h"
#include "Connection.h"

class PE {
public:
    PE(int row = 0, int col = 0);

    void receive();
    void send();
    void compute();

    void setTopConnection(Connection* conn);
    void setLeftConnection(Connection* conn);
    void setRightConnection(Connection* conn);
    void setBottomConnection(Connection* conn);
    void logState(int row, int col, int cycle) const;

    void sendBottom();
    void sendRight();
    void sendPsum();
    void sendTransfer();

    void cycle();

    Connection* getBottomConnection();
    Connection* getRightConnection();
    FIFO<DataPackage> receivedA, receivedB, receivedPsum, Psum, forwardA, forwardB, PsumOut;

    FIFO<DataPackage> receivedTransfer;
    int r,c;
    Connection* connection_top;
    Connection* connection_bottom;
    Connection* connection_left;
    Connection* connection_right;
};

#endif // PE_H
