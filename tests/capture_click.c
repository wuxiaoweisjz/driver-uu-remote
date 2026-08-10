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
        CAPTURE_MAGIC, CAPTURE_INPUT_REQUEST, CAPTURE_PROTOCOL_VERSION, 3
    };
    CaptureInputEvent events[3] = {0};
    CaptureInputReply reply;
    long x, y, width, height;
    int connection;

    if (argc != 5) return 2;
    x = strtol(argv[1], NULL, 10);
    y = strtol(argv[2], NULL, 10);
    width = strtol(argv[3], NULL, 10);
    height = strtol(argv[4], NULL, 10);
    if (x < 0 || y < 0 || width <= 1 || height <= 1 || x >= width || y >= height)
        return 2;

    events[0].type = CAPTURE_INPUT_MOUSE;
    events[0].dx = (int32_t)(x * 65535 / (width - 1));
    events[0].dy = (int32_t)(y * 65535 / (height - 1));
    events[0].flags = 0x8001;
    events[1].type = CAPTURE_INPUT_MOUSE;
    events[1].flags = 0x0002;
    events[2].type = CAPTURE_INPUT_MOUSE;
    events[2].flags = 0x0004;

    connection = socket(AF_INET, SOCK_STREAM, 0);
    if (connection < 0) return 1;
    setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(CAPTURE_DEFAULT_PORT);
    if (connect(connection, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        send_all(connection, &request, sizeof(request)) < 0 ||
        send_all(connection, events, sizeof(events)) < 0 ||
        recv_all(connection, &reply, sizeof(reply)) < 0 ||
        reply.magic != CAPTURE_MAGIC || reply.type != CAPTURE_INPUT_REPLY ||
        reply.protocol_version != CAPTURE_PROTOCOL_VERSION || reply.status ||
        reply.accepted_count != 3) {
        fprintf(stderr, "capture click failed: %s\n", strerror(errno));
        close(connection);
        return 1;
    }
    close(connection);
    printf("capture click accepted at %ld,%ld\n", x, y);
    return 0;
}
