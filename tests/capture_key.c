#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "capture_protocol.h"

static int send_all(int fd, const void *data, size_t size)
{
    const uint8_t *cursor = data;
    while (size) {
        ssize_t count = send(fd, cursor, size, MSG_NOSIGNAL);
        if (count <= 0) return -1;
        cursor += count;
        size -= (size_t)count;
    }
    return 0;
}

static int recv_all(int fd, void *data, size_t size)
{
    uint8_t *cursor = data;
    while (size) {
        ssize_t count = recv(fd, cursor, size, 0);
        if (count <= 0) return -1;
        cursor += count;
        size -= (size_t)count;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct sockaddr_in address = {0};
    struct timeval timeout = {3, 0};
    CaptureInputRequest request = {
        CAPTURE_MAGIC, CAPTURE_INPUT_REQUEST, CAPTURE_PROTOCOL_VERSION, 2
    };
    CaptureInputEvent events[2] = {0};
    CaptureInputReply reply;
    char *end = NULL;
    unsigned long virtual_key;
    int connection;

    if (argc != 2 && argc != 3) return 2;
    virtual_key = strtoul(argv[1], &end, 0);
    if (!end || *end || !virtual_key || virtual_key > UINT16_MAX) return 2;

    events[0].type = CAPTURE_INPUT_KEYBOARD;
    events[0].virtual_key = (uint16_t)virtual_key;
    events[1] = events[0];
    events[1].flags = 0x0002;
    if (argc == 3) {
        request.event_count = 1;
        if (!strcmp(argv[2], "down")) {
            events[0].flags = 0;
        } else if (!strcmp(argv[2], "up")) {
            events[0].flags = 0x0002;
        } else {
            return 2;
        }
    }

    connection = socket(AF_INET, SOCK_STREAM, 0);
    if (connection < 0) return 1;
    setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(CAPTURE_DEFAULT_PORT);
    if (connect(connection, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        send_all(connection, &request, sizeof(request)) < 0 ||
        send_all(connection, events,
                 sizeof(events[0]) * request.event_count) < 0 ||
        recv_all(connection, &reply, sizeof(reply)) < 0 ||
        reply.magic != CAPTURE_MAGIC || reply.type != CAPTURE_INPUT_REPLY ||
        reply.protocol_version != CAPTURE_PROTOCOL_VERSION || reply.status ||
        reply.accepted_count != request.event_count) {
        fprintf(stderr, "capture key failed: %s\n", strerror(errno));
        close(connection);
        return 1;
    }
    close(connection);
    printf("capture key accepted: 0x%lx\n", virtual_key);
    return 0;
}
