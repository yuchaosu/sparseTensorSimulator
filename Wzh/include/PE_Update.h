#ifndef PE_UPDATE_H
#define PE_UPDATE_H

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

    void setLastRow(bool last);
    void setLastCol(bool last);

    void setTopConnection(Connection* conn);
    void setLeftConnection(Connection* conn);
    void setRightConnection(Connection* conn);
    void setBottomConnection(Connection* conn);
    void logState(int row, int col, int cycle) const;

    void sendBottom();
    void sendRight();
    void sendPsum();
    void sendIsInjectionFinishedLeft();
    void sendIsInjectionFinishedBottom();
    //void sendTransfer();
    //void sendReceivedPsum();

    void cycle();

    bool isIdle() const;
    void setIdle(bool idle);


    Connection* getBottomConnection();
    Connection* getRightConnection();
    FIFO<DataPackage> receivedA; //Received data from the top connection
    FIFO<DataPackage> receivedB; //Received data from the left connection
    //FIFO<DataPackage> receivedPsum; //Received partial sums from other PEs
    //FIFO<DataPackage> receivedTransfer; //Received transferred partial sums
    //FIFO<DataPackage> receivedPsumOut; //Output partial sum, combination of Psum and receivedTransfer
    FIFO<DataPackage> PsumOut; //Current partial sum
    //FIFO<DataPackage> transferOut; //Current transfer data

    int r,c;
    Connection* connection_top;
    Connection* connection_bottom;
    Connection* connection_left;
    Connection* connection_right;
    private:
    bool idle = false; // Indicates if the PE is idle
    std::ostream& out;
    bool blocked_A = false; // Indicates if PE is blocked on A
    bool blocked_B = false; // Indicates if PE is blocked on B
    bool sent_A = false; // Indicates if PE has sent A
    bool sent_B = false; // Indicates if PE has sent B
    bool injection_finished_left = false; // Indicates if all data has been injected
    bool injection_finished_top = false; // Indicates if all data has been injected
    bool last_row = false;
    bool last_col = false; // Indicates if this is the last column
    bool sent_finished_left = false; // Indicates if the left connection has been sent
    bool sent_finished_top = false; // Indicates if the top connection has been sent
};

#endif // PE_H
