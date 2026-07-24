#include "broker.h"

#include <stdio.h>

int main(int argc, char **argv) {
    const char *host = "0.0.0.0";
    const char *port = "1883";

    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        port = argv[2];
    }
    if (argc > 3) {
        fprintf(stderr, "Usage: %s [host] [port]\n", argv[0]);
        return 2;
    }
    return broker_start(host, port);
}
