#include "../include/PE_Update.h"

PE::PE(int row, int col, std::ostream& output_stream) : r(row), c(col), out(output_stream) {
    connection_top = nullptr;
    connection_bottom = nullptr;
    connection_left = nullptr;
    connection_right = nullptr;
    out << "PE created at (" << r << ", " << c << ")\n";
}

void PE::setLastRow(bool last) {
    last_row = last;
}

void PE::setLastCol(bool last) {
    last_col = last;
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
            sent_A = true; // Mark A as sent
        } else {
            
            if (last_row)  {
                receivedA.pop();
                out << "PE (" << r << ", " << c << ") is the last row, drop the value from FIFO A\n";
            } else {
                out << "PE (" << r << ", " << c << ") cannot send to bottom yet, waiting for previous data to be processed.\n";
            }
        }
    }
}

void PE::sendRight() {
    if (!receivedB.isEmpty() && connection_right) {
        DataPackage val = receivedB.front();
        if( !connection_right->pendingSrc()) {
            // If the right connection is not pending, we can send the value
            connection_right->receiveSrc(val);
            out << "PE (" << r << ", " << c << ") sending to right: " << val.value << " \t index1: " << val.index1 << " \t index2: " << val.index2 << "\n";
            sent_B = true; // Mark B as sent
        } else {
            // If the right connection is pending, we might need to handle it differently
            // For now, we just print a message
            if (last_col) {
                receivedB.pop();
                out << "PE (" << r << ", " << c << ") is the last column, drop the value from FIFO B\n";
            }
            else
            out << "PE (" << r << ", " << c << ") cannot send to right yet, waiting for previous data to be processed.\n";
        }
        
    }
}

void PE::sendPsum() {
    if (!PsumOut.isEmpty() && connection_bottom) {
        DataPackage val = PsumOut.front();
        connection_bottom->receivePsum(val);
        out << "PE (" << r << ", " << c << ") sending Psum to Reduction: " << val.value << " \t index1: " << val.index1 << " \t index2: " << val.index2 << "\n";
        PsumOut.pop();
    }
}

void PE::sendIsInjectionFinishedLeft() {
    if (connection_right && injection_finished_left && !blocked_B && !handshake_finished_right) {
        if (!sent_finished_left) {
            connection_right->receiveInjectionFinished(injection_finished_left);
            out << "PE (" << r << ", " << c << ") sending injection finished to right: " << injection_finished_left << "\n";
            sent_finished_left = true; // Mark that we have sent the injection finished signal to the right connection
        }
    }
}

void PE::sendIsInjectionFinishedBottom() {
    if (connection_bottom && injection_finished_top && !last_row && !blocked_A && !handshake_finished_bottom) {
        if(!sent_finished_top) {
            connection_bottom->receiveInjectionFinished(injection_finished_top);
            out << "PE (" << r << ", " << c << ") sending injection finished to bottom: " << injection_finished_top << "\n";
            sent_finished_top = true; // Mark that we have sent the injection finished signal to the bottom connection
        }
    }
}

void PE::receive() {

    if (connection_top && connection_top->pendingInjectionFinished()) {
        if (receivedA.isFull()) {
            out << "PE (" << r << ", " << c << ") FIFO A is full, cannot receive injection finished.\n";
        } else {
            injection_finished_top = connection_top->isInjectionFinished();
            out << "PE (" << r << ", " << c << ") received injection finished from top: " << injection_finished_top << "\n";
        }
    }

    if (connection_top && connection_top->pendingSrc()) {
        
        
        if (receivedA.isFull()) {
            out << "PE (" << r << ", " << c << ") FIFO A is full, cannot receive more data.\n";
        } else {
            
            DataPackage src = connection_top->sendSrc();
            receivedA.push(src);
            sent_A = false; // Reset sent_A flag
            out << "PE (" << r << ", " << c << ") received A from top: " << src.value << " \t index1: " << src.index1 << " \t index2: " << src.index2 << "\n";
        }
    }


    if (connection_left && connection_left->pendingInjectionFinished()) {
        if (receivedB.isFull()) {
            out << "PE (" << r << ", " << c << ") FIFO B is full, cannot receive injection finished.\n";
        } else {
            injection_finished_left = connection_left->isInjectionFinished();
            out << "PE (" << r << ", " << c << ") received injection finished from left: " << injection_finished_left << "\n";
        }
    }

    if (connection_left && connection_left->pendingSrc()) {

        if (receivedB.isFull()) {
            out << "PE (" << r << ", " << c << ") FIFO B is full, cannot receive more data.\n";
        } else {
            
            
            DataPackage src = connection_left->sendSrc();
            receivedB.push(src);
            sent_B = false; // Reset sent_B flag
            out << "PE (" << r << ", " << c << ") received B from left: " << src.value << " \t index1: " << src.index1 << " \t index2: " << src.index2 << "\n";
        }
    }


    


    
}
void PE::cycle() {
    //idle = true; // Reset idle state at the start of the cycle
    finishHandshakeReceive();
    if (!receivedA.isEmpty() && !receivedB.isEmpty()) {
        DataPackage valueA = receivedA.front();
        DataPackage valueB = receivedB.front();
        if (valueA.index2 == valueB.index1) {
            DataPackage result = DataPackage(valueA.value * valueB.value, valueA.index1, valueB.index2);
            PsumOut.push(result);
            out << "PE (" << r << ", " << c << ") computed multiplication: " << valueA.value << " * " << valueB.value << " = " << result.value << " \t index1: " << valueA.index1 << " \t index2: " << valueB.index2 << "\n";
            // if (blocked_A) {
            //     blocked_A = false; // Unblock A
            // }
            // else {
            if( !sent_A) {
                sendBottom();
            }
            // }
            // if (blocked_B) {
            //     blocked_B = false; // Unblock B
            // }
            // else {
            //     out<< "PE (" << r << ", " << c << ") sending B to right after computation.\n";
            if( !sent_B) {
                sendRight();
            //}
            }
            blocked_A = false; // Unblock A
            blocked_B = false; // Unblock B
            if (!receivedA.isEmpty()) {
                receivedA.pop();
            }
            if (!receivedB.isEmpty()) {
                receivedB.pop();
            }
            sendIsInjectionFinishedLeft();
            sendIsInjectionFinishedBottom();
        } else if (valueA.index2 < valueB.index1) {
            out << "PE (" << r << ", " << c << ") Index mismatch, block the large one, B: " << valueB.value << " " << valueB.index1 << " " << valueB.index2 << "\n";
            blocked_B = true; // Block B
            sendBottom();
            if (!sent_B) {
                // If B was not sent yet, we send it now
                sendRight();
            }

            if (!receivedA.isEmpty()) {
                receivedA.pop();
            }
            if (injection_finished_top) {
                blocked_B = false; // Unblock B
                if(!receivedB.isEmpty()) {
                    receivedB.pop(); // It is the last element in A, no more matches, so we can pop B
                }
                sendIsInjectionFinishedLeft();
            }

        } else if (valueA.index2 > valueB.index1) {
            out << "PE (" << r << ", " << c << ") Index mismatch, block the large one, A: " << valueA.value << " " << valueA.index1 << " " << valueA.index2 << "\n";
            blocked_A = true; // Block A
            sendRight();
            if (!sent_A) {
                // If A was not sent yet, we send it now
                sendBottom();
            }

            if (!receivedB.isEmpty()) {
                receivedB.pop();
            }
            if (injection_finished_left) {
                blocked_A = false; // Unblock A
                if(!receivedA.isEmpty()) {
                    receivedA.pop(); // It is the last element in B, no more matches, so we can pop A
                }
                sendIsInjectionFinishedBottom();
            }
        }
    }
    else if (!receivedB.isEmpty()) {
        out << "PE (" << r << ", " << c << ") does not receive A\n";
        if (!sent_B) {
            sendRight();
             // Reset idle state when processing data
            //receivedB.pop();
        }
        if (injection_finished_top && !receivedB.isEmpty()) {
            blocked_B = false;
            receivedB.pop();
            out << "PE (" << r << ", " << c << ") reach to the end of A. Drop B\n";
            sendIsInjectionFinishedLeft();
            sendIsInjectionFinishedBottom();
        }


    }
    else if (!receivedA.isEmpty()) {
        out << "PE (" << r << ", " << c << ") does not receive B\n";
        if (!sent_A) {
            sendBottom();
             // Reset idle state when processing data
            //receivedA.pop();
        }
        if (injection_finished_left && !receivedA.isEmpty()) {
            blocked_A = false;
            receivedA.pop(); // If we are at the left column, we can pop A directly
            out << "PE (" << r << ", " << c << ") reach to the end of B. Drop A\n";
            sendIsInjectionFinishedBottom();
            sendIsInjectionFinishedLeft();
        }

    }
    sendPsum();
    if (injection_finished_left && injection_finished_top && PsumOut.isEmpty()) {
        idle = true; 
    }
    else {
        idle = false; // Set idle state if no data is being processed
    }
    out << "PE (" << r << ", " << c << ") idle state: " << (idle ? "true" : "false") << "\n";
    out << "PE (" << r << ", " << c << ") current state: Left Injection" << injection_finished_left 
        << " Top Injection" << injection_finished_top << " PsumOut" << PsumOut.isEmpty() << "\n";
    if(idle) {
        sendIsInjectionFinishedLeft();
        sendIsInjectionFinishedBottom();
    }
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

void PE::finishHandshakeSend() {
    if (idle) {
        if (injection_finished_top && connection_top) {
            connection_top->receiveInjectionFinished(injection_finished_top);
            out << "PE (" << r << ", " << c << ") sending injection finished handshake to top\n";
        }

        if (injection_finished_left && connection_left) {
            connection_left->receiveInjectionFinished(injection_finished_left);
            out << "PE (" << r << ", " << c << ") sending injection finished handshake to left\n";
        }
    }
}

void PE::finishHandshakeReceive() {
    if (connection_bottom && connection_bottom->pendingHandshakeFinished()) {
        handshake_finished_bottom = connection_bottom->isHandshakeFinished();
        out << "PE (" << r << ", " << c << ") received handshake finished from bottom: " << handshake_finished_bottom << "\n";
        
    }
    if (connection_right && connection_right->pendingHandshakeFinished()) {
        handshake_finished_right = connection_right->isHandshakeFinished();
        out << "PE (" << r << ", " << c << ") received handshake finished from right: " << handshake_finished_right << "\n";
    }
}