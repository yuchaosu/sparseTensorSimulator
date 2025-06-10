//Created 13/06/2019

#include "connection.h"
#include <iostream>
#include <assert.h>

using namespace std;

Connection::Connection() {
    this->pending_src = false;
    this->pending_psum = false;
    this->pending_transfer = false;
    sends = 0;
    receives = 0;
}



//Send a package to the interconnection. If there is no remaining bandiwth an exception is raised
void Connection::send() {
    this->src = src;
    this->psum = psum;
    this->transfer = transfer;
    this->pending_src = false;
    this->pending_psum = false;
    this->pending_transfer = false;

    return;
}

//Return the packages from the interconnection
void Connection::receive(DataPackage src, DataPackage psum, DataPackage transfer) {
    assert(!this->pending_src && "Connection already has pending src data. Cannot receive new data until the previous is sent."); 
    this->src = src;
    this->pending_src = true;
    assert(!this->pending_psum && "Connection already has pending psum data. Cannot receive new data until the previous is sent.");
    this->psum = psum;
    this->pending_psum = true;
    assert(!this->pending_transfer && "Connection already has pending transfer data. Cannot receive new data until the previous is sent.");
    this->transfer = transfer;
    this->pending_transfer = true;
}

void Connection::receiveSrc(DataPackage src) {
    assert(!this->pending_src && "Already has buffered src data.");
    this->src = src;
    this->pending_src = true;
    printf("Connection: received src data: %f, index1: %d, index2: %d\n", src.value, src.index1, src.index2);
    receives++;
}
void Connection::receivePsum(DataPackage psum) {
    assert(!this->pending_psum && "Already has buffered psum data.");
    this->psum = psum;
    this->pending_psum = true;
    printf("Connection: received psum data: %f, index1: %d, index2: %d\n", psum.value, psum.index1, psum.index2);
    receives++;
}

void Connection::receiveTransfer(DataPackage transfer) {
    assert(!this->pending_transfer && "Already has buffered transfer data.");
    this->transfer = transfer;
    this->pending_transfer = true;
    printf("Connection: received transfer data: %f, index1: %d, index2: %d\n", transfer.value, transfer.index1, transfer.index2);
    receives++;
}

DataPackage Connection::sendSrc() {
    if (this->pending_src) {
        this->sends++;
        this->pending_src = false;
        return this->src;
    }
    else {
        assert(false && "No pending src data to send.");
        return DataPackage();  // Return an empty package if no pending data
    }
}

DataPackage Connection::sendPsum() {
    if (this->pending_psum) {
        this->sends++;
        this->pending_psum = false;
        return this->psum;
    }
    else {
        assert(false && "No pending psum data to send.");
        return DataPackage();  // Return an empty package if no pending data
    }
}

DataPackage Connection::sendTransfer() {
    if (this->pending_transfer) {
        this->sends++;
        this->pending_transfer = false;
        return this->transfer;
    }
    else {
        assert(false && "No pending transfer data to send.");
        return DataPackage();  // Return an empty package if no pending data
    }
}

bool Connection::pendingSrc() {
    return pending_src;
}

bool Connection::pendingPsum() {
    return pending_psum;
}

bool Connection::pendingTransfer() {
    return pending_transfer;
}

void Connection::printEnergy() {
    printf("Connection: sends=%zu, receives=%zu\n", sends, receives);
}




