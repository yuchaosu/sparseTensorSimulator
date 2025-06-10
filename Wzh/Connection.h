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
    bool pending_src;   // Indicates if data exists
    bool pending_psum;
    bool pending_transfer;
    //size_t bw;           // Size in bytes of actual data. In the simulator this size is greater since we wrap the data into wrappers to track.
    DataPackage src;   // Array of packages that are send/receive in  a certain cycle. The number of packages depends on the bw of the connection
    DataPackage psum;
    DataPackage transfer;
    size_t sends; // Number of sends
    size_t receives; // Number of receives

    DataPackage delayed_src;   // Used to store the src package that is sent in the next cycle
    DataPackage delayed_psum;  // Used to store the psum package that is sent in the next cycle
    DataPackage delayed_transfer;  // Used to store the transfer package that is sent in the next cycle

    bool is_delayed_src = false;   // Indicates if the src package is delayed
    bool is_delayed_psum = false;  // Indicates if the psum package is delayed
    bool is_delayed_transfer = false;  // Indicates if the transfer package is delayed
public:
    Connection();
    void send(); //Package of data to be send. The sum of all the size_package of each package must not be greater than bw.
    void receive(DataPackage src, DataPackage psum, DataPackage transfer);  //Receive a  packages from the connection
    void receiveSrc(DataPackage src);
    void receivePsum(DataPackage psum);
    void receiveTransfer(DataPackage transfer);

    DataPackage sendSrc();
    DataPackage sendPsum();
    DataPackage sendTransfer();

    bool pendingSrc();
    bool pendingPsum();
    bool pendingTransfer();
    void printEnergy();

    void cycle();  // Cycle the connection, sending delayed packages if any
};


#endif

