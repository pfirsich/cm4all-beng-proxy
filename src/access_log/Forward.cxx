/*
 * An access logger which forwards all datagrams to one or more remote
 * loggers via UDP or a local datagram socket.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "net/UniqueSocketDescriptor.hxx"
#include "net/RConnectSocket.hxx"
#include "util/PrintException.hxx"

#include <array>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>

struct Destination {
    UniqueSocketDescriptor fd;
    bool failed;
};

int main(int argc, char **argv)
try {
    if (argc < 2) {
        fprintf(stderr, "Usage: log-forward HOST ...\n");
        return EXIT_FAILURE;
    }

    static std::array<Destination, 256> destinations;
    unsigned num_destinations = argc - 1;

    if (num_destinations > destinations.size()) {
        fprintf(stderr, "Too many hosts\n");
        return EXIT_FAILURE;
    }

    for (unsigned i = 0; i < num_destinations; ++i) {
        destinations[i].fd = ResolveConnectDatagramSocket(argv[1 + i], 5479);
    }

    static char buffer[16384];
    ssize_t nbytes;
    while ((nbytes = recv(0, buffer, sizeof(buffer), 0)) > 0) {
        size_t length = (size_t)nbytes;
        for (unsigned i = 0; i < num_destinations; ++i) {
            nbytes = destinations[i].fd.Write(buffer, length);
            if (nbytes == (ssize_t)length)
                destinations[i].failed = false;
            else if (nbytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                     !destinations[i].failed) {
                fprintf(stderr, "send() to host %u failed: %s\n",
                        i, strerror(errno));
                destinations[i].failed = true;
            }
        }
    }

    return 0;
} catch (const std::exception &e) {
    PrintException(e);
    return EXIT_FAILURE;
}
