//Created 13/06/2019

#ifndef __Connection__h
#define __Connection__h

#include <vector>
#include "Utility.h"


/*
This class Connection does not need ACK responses since in the accelerator the values are sent without a need of a request. Everything is controlled
by the control of the accelerator. 
*/


class Connection {
private:
    bool pending_src = false;   // Indicates if data exists
    bool pending_psum = false;
    bool pending_transfer = false;
    bool pending_injection_finished = false; // Indicates if the injection of data is finished
    bool pending_handshake_finished = false; // Indicates if the handshake is finished
    //size_t bw;           // Size in bytes of actual data. In the simulator this size is greater since we wrap the data into wrappers to track.
    DataPackage src;   // Array of packages that are send/receive in  a certain cycle. The number of packages depends on the bw of the connection
    DataPackage psum;
    DataPackage transfer;
    bool injection_finished; // Indicates if the injection of data is finished
    bool handshake_finished; // Indicates if the handshake is finished
    size_t sends = 0; // Number of sends
    size_t receives = 0; // Number of receives

    std::ostream& out; // Output stream for logging

public:
    Connection(std::ostream& out);
    void send(); //Package of data to be send. The sum of all the size_package of each package must not be greater than bw.
    void receive(DataPackage src, DataPackage psum, DataPackage transfer);  //Receive a  packages from the connection
    void receiveSrc(DataPackage src);
    void receivePsum(DataPackage psum);
    void receiveTransfer(DataPackage transfer);
    void receiveInjectionFinished(bool finished);
    void receiveHandshakeFinished(bool finished);

    DataPackage sendSrc();
    DataPackage sendPsum();
    DataPackage sendTransfer();
    bool isInjectionFinished();
    bool isHandshakeFinished();

    bool pendingSrc();
    bool pendingPsum();
    bool pendingTransfer();
    bool pendingInjectionFinished();
    bool pendingHandshakeFinished();
    void printEnergy(std::ostream& out) const;

};


#endif

