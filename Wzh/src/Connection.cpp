

#include "../include/Connection.h"
#include <iostream>
#include <assert.h>

using namespace std;

Connection::Connection(std::ostream& out) : out(out) {
    this->pending_src = false;
    this->pending_psum = false;
    this->pending_transfer = false;
    sends = 0;
    receives = 0;
}



//Send a package to the interconnection. If there is no remaining bandwidth an exception is raised
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
    if(this->pending_src) {
        out << "Connection already has pending src data. Cannot receive new data until the previous is sent." << endl;
        return;
    }
    this->src = src;
    this->pending_src = true;
    if(this->pending_psum) {
        out << "Connection already has pending psum data. Cannot receive new data until the previous is sent." << endl;
        return;
    }
    this->psum = psum;
    this->pending_psum = true;
    if(this->pending_transfer) {
        out << "Connection already has pending transfer data. Cannot receive new data until the previous is sent." << endl;
        return;
    }
    this->transfer = transfer;
    this->pending_transfer = true;
}

void Connection::receiveSrc(DataPackage src) {
    if(this->pending_src) {
        out << "Connection already has buffered src data. Cannot receive new data until the previous is sent." << endl;
        return;
    }
    this->src = src;
    this->pending_src = true;
    out << "Connection: received src data: " << src.value << ", index1: " << src.index1 << ", index2: " << src.index2 << endl;
    receives++;
}
void Connection::receivePsum(DataPackage psum) {
    if(this->pending_psum) {
        out << "Connection already has buffered psum data. Cannot receive new data until the previous is sent." << endl;
        return;
    }
    this->psum = psum;
    this->pending_psum = true;
    out << "Connection: received psum data: " << psum.value << ", index1: " << psum.index1 << ", index2: " << psum.index2 << endl;
    receives++;
}

void Connection::receiveTransfer(DataPackage transfer) {
    if(this->pending_transfer) {
        out << "Connection already has buffered transfer data. Cannot receive new data until the previous is sent." << endl;
        return;
    }
    this->transfer = transfer;
    this->pending_transfer = true;
    out << "Connection: received transfer data: " << transfer.value << ", index1: " << transfer.index1 << ", index2: " << transfer.index2 << endl;
    receives++;
}

DataPackage Connection::sendSrc() {
    if (this->pending_src) {
        this->sends++;
        this->pending_src = false;
        return this->src;
    }
    else {
        out << "No pending src data to send." << endl;
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
        out << "No pending psum data to send." << endl;
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
        out << "No pending transfer data to send." << endl;
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
    out << "Connection: sends=" << sends << ", receives=" << receives << endl;
}




