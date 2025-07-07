#include "../include/PE.h"

PE::PE(int row, int col, std::ostream& output_stream) : r(row), c(col), out(output_stream) {
    connection_top = nullptr;
    connection_bottom = nullptr;
    connection_left = nullptr;
    connection_right = nullptr;
    out << "PE created at (" << r << ", " << c << ")\n";
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
            out << "PE (" << r << ", " << c << ") sending to bottom: " << val.value << " \t index1: " << val.index1 << " \t index2: " << val.index2 << "\n";
            sends++;
        } else {
            // If the bottom connection is pending, we might need to handle it differently
            // For now, we just print a message
            out << "PE (" << r << ", " << c << ") cannot send to bottom yet, waiting for previous data to be processed.\n";
        }
    }
}

void PE::sendPsum() {
    if (!PsumOut.isEmpty() && connection_bottom) {
        DataPackage val = PsumOut.front();
        connection_bottom->receivePsum(val);
        out << "PE (" << r << ", " << c << ") sending Psum to bottom: " << val.value << " \t index1: " << val.index1 << " \t index2: " << val.index2 << "\n";
        PsumOut.pop();
        sends++;
    }
}


void PE::sendRight() {
    if (!receivedB.isEmpty() && connection_right) {
        DataPackage val = receivedB.front();
        if( !connection_right->pendingSrc()) {
            // If the right connection is not pending, we can send the value
            connection_right->receiveSrc(val);
            out << "PE (" << r << ", " << c << ") sending to right: " << val.value << " \t index1: " << val.index1 << " \t index2: " << val.index2 << "\n";
            sends++;
        } else {
            // If the right connection is pending, we might need to handle it differently
            // For now, we just print a message
            out << "PE (" << r << ", " << c << ") cannot send to right yet, waiting for previous data to be processed.\n";
        }
        
    }
}


void PE::receive() {
    if (connection_top && connection_top->pendingSrc()) {
        DataPackage src = connection_top->sendSrc();
        //if (src.value != INT_MIN) {
            receivedA.push(src);
            out << "PE (" << r << ", " << c << ") received A from top: " << src.value << " \t index1: " << src.index1 << " \t index2: " << src.index2 << "\n";

            receives++;
        

    }

    if (connection_left && connection_left->pendingSrc()) {
        DataPackage psum = connection_left->sendSrc();
        //if (psum.value != INT_MIN) {
            receivedB.push(psum);
            out << "PE (" << r << ", " << c << ") received B from left: " << psum.value << " \t index1: " << psum.index1 << " \t index2: " << psum.index2 << "\n";

            receives++;
        

    }

    // if (connection_top && connection_top->pendingPsum()) {
    //     DataPackage psum = connection_top->sendPsum();
    //     if (psum.value != INT_MIN) {
    //         PsumOut.push(psum);
    //         out << "PE (" << r << ", " << c << ") received Psum from top: " << psum.value << " \t index1: " << psum.index1 << " \t index2: " << psum.index2 << "\n";
    //     }
    // }

    if (connection_top && connection_top->pendingTransfer()) {
        DataPackage transfer = connection_top->sendTransfer();
        //if (transfer.value != INT_MIN) {
            PsumOut.push(transfer);
            out << "PE (" << r << ", " << c << ") received Transfer from top: " << transfer.value << " \t index1: " << transfer.index1 << " \t index2: " << transfer.index2 << "\n";

            receives++;
        

    }
    
}
void PE::cycle() {
    idle = true; // Reset idle state at the start of the cycle

    if (!receivedA.isEmpty() || !receivedB.isEmpty() || !PsumOut.isEmpty()) {
        idle = false; // If there is any data to process, the PE is not idle
    }

    if (!receivedA.isEmpty() && !receivedB.isEmpty()) {
        DataPackage valueA = receivedA.front();
        DataPackage valueB = receivedB.front();
        if (valueA.index2 == valueB.index1) {
            DataPackage result = DataPackage(valueA.value * valueB.value, valueA.index1, valueB.index2);
            PsumOut.push(result);
            out << "PE (" << r << ", " << c << ") computed multiplication: " << valueA.value << " * " << valueB.value << " = " << result.value << " \t index1: " << valueA.index1 << " \t index2: " << valueB.index2 << "\n";
            sendBottom();
            sendRight();
            receivedA.pop();
            receivedB.pop();
            multiplies++;
            compares++;
        } else if (valueA.index2 < valueB.index1) {
            out << "PE (" << r << ", " << c << ") Index mismatch, block the large one, B: " << valueB.value << " " << valueB.index1 << " " << valueB.index2 << "\n";
            sendBottom();
            #ifdef PARALLEL
            sendRight();
            #endif
            receivedA.pop();
            compares++;
        } else if (valueA.index2 > valueB.index1) {
            out << "PE (" << r << ", " << c << ") Index mismatch, block the large one, A: " << valueA.value << " " << valueA.index1 << " " << valueA.index2 << "\n";
            sendRight();
            #ifdef PARALLEL
            sendBottom();
            #endif
            receivedB.pop();
            compares++;
        }
    }
    else if (!receivedB.isEmpty()) {
        out << "PE (" << r << ", " << c << ") does not receive A\n";
        sendRight();
        receivedB.pop();
    }
    else if (!receivedA.isEmpty()) {
        out << "PE (" << r << ", " << c << ") does not receive B\n";
        sendBottom();
        receivedA.pop();
    }

    sendPsum();
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

void PE::printEnergy(std::ostream& out) const {
    out << "Multiplies: " << multiplies << "\n";
    out << "Compares: " << compares << "\n";
    out << "Sends: " << sends << "\n";
    out << "Receives: " << receives << "\n";
}