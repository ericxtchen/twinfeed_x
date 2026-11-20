#ifndef UDP_RECEIVER_H
#define UDP_RECEIVER_H

#include "receiver.h"

// replay_fd is an optional UDP socket connected to the publisher's replay port.
std::optional<ReplayRequest> receive_once(Receiver& receiver, int fd_a, int fd_b,
    std::chrono::steady_clock::duration gap_wait, int replay_fd = -1);

#endif
