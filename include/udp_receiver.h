#ifndef UDP_RECEIVER_H
#define UDP_RECEIVER_H

#include "../include/receiver.h"
std::optional<ReplayRequest> receive_once(Receiver &receiver, int fd_a, int fd_b, std::chrono::steady_clock::duration gap_wait); 

#endif
