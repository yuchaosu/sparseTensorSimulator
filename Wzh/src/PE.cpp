#include "../include/PE.h"

PE::PE(int row, int col) : r(row), c(col) {
    connection_top = nullptr;
    connection_bottom = nullptr;
    connection_left = nullptr;
    connection_right = nullptr;
    printf("PE created at (%d, %d)\n", r, c);
}


void PE::setTopConnection(Connection* conn) {
    connection_top = conn;
}

void PE::setLeftConnection(Connection* conn) {
    connection_left = conn;
}
void PE::setRightConnection(Connection* conn) {
    connection_right = conn;
}
void PE::setBottomConnection(Connection* conn) {
    connection_bottom = conn;
}


void PE::sendBottom() {
    if (!receivedA.isEmpty() && connection_bottom) {
        DataPackage val = receivedA.front();
        if (!connection_bottom->pendingSrc()) {
            // If the bottom connection is not pending, we can send the value
            connection_bottom->receiveSrc(val);
            printf("PE (%d, %d) sending to bottom: %f \t index1: %d \t index2: %d\n", r, c, val.value, val.index1, val.index2);
        } else {
            // If the bottom connection is pending, we might need to handle it differently
            // For now, we just print a message
            std::cerr << "PE (" << r << ", " << c << ") cannot send to bottom yet, waiting for previous data to be processed.\n";
        }
    }
}

void PE::sendPsum() {
    if (!Psum.isEmpty() && connection_bottom) {
        DataPackage val = Psum.front();
        connection_bottom->receivePsum(val);
        printf("PE (%d, %d) sending Psum to bottom: %f \t index1: %d \t index2: %d\n", r, c, val.value, val.index1, val.index2);
        Psum.pop();
    }
}

void PE::sendTransfer() {
    if (!PsumOut.isEmpty() && connection_bottom) {
        DataPackage val = PsumOut.front();
        connection_bottom->receiveTransfer(val);
        printf("PE (%d, %d) sending Transfer to bottom: %f \t index1: %d \t index2: %d\n", r, c, val.value, val.index1, val.index2);
        PsumOut.pop();
    }
}

void PE::sendRight() {
    if (!receivedB.isEmpty() && connection_right) {
        DataPackage val = receivedB.front();
        if( !connection_right->pendingSrc()) {
            // If the right connection is not pending, we can send the value
            connection_right->receiveSrc(val);
            printf("PE (%d, %d) sending to right: %f \t index1: %d \t index2: %d\n", r, c, val.value, val.index1, val.index2);
        } else {
            // If the right connection is pending, we might need to handle it differently
            // For now, we just print a message
            std::cerr << "PE (" << r << ", " << c << ") cannot send to right yet, waiting for previous data to be processed.\n";
        }
        
    }
}


void PE::receive() {
    if (connection_top && connection_top->pendingSrc()) {
        DataPackage src = connection_top->sendSrc();
        if (src.value != INT_MIN) {
            receivedA.push(src);
            printf("PE (%d, %d) received A from top: %f \t index1: %d \t index2: %d\n", r, c, src.value, src.index1, src.index2);
        }
    }

    if (connection_left && connection_left->pendingSrc()) {
        DataPackage psum = connection_left->sendSrc();
        if (psum.value != INT_MIN) {
            receivedB.push(psum);
            printf("PE (%d, %d) received B from left: %f \t index1: %d \t index2: %d\n", r, c, psum.value, psum.index1, psum.index2);
        } 
    }

    if (connection_top && connection_top->pendingPsum()) {
        DataPackage psum = connection_top->sendPsum();
        if (psum.value != INT_MIN) {
            PsumOut.push(psum);
            printf("PE (%d, %d) received Psum from top: %f \t index1: %d \t index2: %d\n", r, c, psum.value, psum.index1, psum.index2);
        }
    }

    if (connection_top && connection_top->pendingTransfer()) {
        DataPackage transfer = connection_top->sendTransfer();
        if (transfer.value != INT_MIN) {
            PsumOut.push(transfer);
            printf("PE (%d, %d) received Transfer from top: %f \t index1: %d \t index2: %d\n", r, c, transfer.value, transfer.index1, transfer.index2);
        }
    }
    
}
void PE::cycle() {
    idle = true; // Reset idle state at the start of the cycle

    if (!receivedA.isEmpty() || !receivedB.isEmpty() || !Psum.isEmpty() || !PsumOut.isEmpty()) {
        idle = false; // If there is any data to process, the PE is not idle
    }

    if (!receivedA.isEmpty() && !receivedB.isEmpty()) {
        DataPackage valueA = receivedA.front();
        DataPackage valueB = receivedB.front();
        if (valueA.index2 == valueB.index1) {
            DataPackage result = DataPackage(valueA.value * valueB.value, valueA.index1, valueB.index2);
            Psum.push(result);
            printf("PE (%d, %d) computed multiplication: %f * %f = %f \t index1: %d \t index2: %d\n", r, c, valueA.value, valueB.value, result.value, valueA.index1, valueB.index2);
            sendBottom();
            sendRight();
            receivedA.pop();
            receivedB.pop();
        } else if (valueA.index2 < valueB.index1) {
            printf("PE (%d, %d) Index mismatch, block the large one, B: %f %d %d\n", r, c, valueB.value, valueB.index1, valueB.index2);
            sendBottom();
            receivedA.pop();
        } else if (valueA.index2 > valueB.index1) {
            printf("PE (%d, %d) Index mismatch, block the large one, A: %f %d %d\n", r, c, valueA.value, valueA.index1, valueA.index2);
            sendRight();
            receivedB.pop();
        }
    }
    else if (!receivedB.isEmpty()) {
        printf("PE (%d, %d) does not receive A\n", r, c);
        sendRight();
        receivedB.pop();
    }
    else if (!receivedA.isEmpty()) {
        printf("PE (%d, %d) does not receive B\n", r, c);
        sendBottom();
        receivedA.pop();
    }

    sendPsum();
    sendTransfer();
    receive(); // Receive new data for the next cycle
    


}

Connection* PE::getBottomConnection() {
    return connection_bottom;
}

Connection* PE::getRightConnection() {
    return connection_right;
}

bool PE::isIdle() const {
    return idle;
}

void PE::setIdle(bool idle) {
    this->idle = idle;
}
