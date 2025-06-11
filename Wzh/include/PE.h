#ifndef PE_H
#define PE_H

#include <iostream>
#include <optional>
#include "Utility.h"
#include "Fifo.h"
#include "Connection.h"

class PE {
public:
    PE(int row = 0, int col = 0, std::ostream& output_stream = std::cout);

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

    bool isIdle() const;
    void setIdle(bool idle);


    Connection* getBottomConnection();
    Connection* getRightConnection();
    FIFO<DataPackage> receivedA; //Received data from the top connection
    FIFO<DataPackage> receivedB; //Received data from the left connection
    FIFO<DataPackage> receivedPsum; //Received partial sums from other PEs
    FIFO<DataPackage> Psum; //Current partial sum
    FIFO<DataPackage> receivedTransfer; //Received transferred partial sums
    FIFO<DataPackage> PsumOut; //Output partial sum, combination of Psum and receivedTransfer

    int r,c;
    Connection* connection_top;
    Connection* connection_bottom;
    Connection* connection_left;
    Connection* connection_right;

    private:
    bool idle = true; // Indicates if the PE is idle
    std::ostream& out;
};

#endif // PE_H
