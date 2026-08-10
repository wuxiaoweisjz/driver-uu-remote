#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
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
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
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
        if (count == 0) return -1;
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        cursor += count;
        size -= (size_t)count;
    }
    return 0;
}

static int authenticate(int fd)
{
    const char *token = getenv("UU_CAPTURE_AUTH_TOKEN");
    CaptureAuthRequest request = {0};
    CaptureAuthReply reply;
    if (!token || !*token) return 0;
    if (strlen(token) != CAPTURE_AUTH_TOKEN_SIZE) return -1;
    request.magic = CAPTURE_MAGIC;
    request.type = CAPTURE_AUTH_REQUEST;
    request.protocol_version = CAPTURE_PROTOCOL_VERSION;
    request.token_size = CAPTURE_AUTH_TOKEN_SIZE;
    memcpy(request.token, token, CAPTURE_AUTH_TOKEN_SIZE);
    if (send_all(fd, &request, sizeof(request)) < 0 ||
        recv_all(fd, &reply, sizeof(reply)) < 0 ||
        reply.magic != CAPTURE_MAGIC || reply.type != CAPTURE_AUTH_REPLY ||
        reply.protocol_version != CAPTURE_PROTOCOL_VERSION || reply.status)
        return -1;
    memset(request.token, 0, sizeof(request.token));
    return 0;
}

static int request_frame(int fd)
{
    const CaptureRequest request = {
        CAPTURE_MAGIC, CAPTURE_REQUEST, CAPTURE_PROTOCOL_VERSION, 0
    };
    CaptureReply reply;
    uint8_t buffer[16384];
    uint32_t remaining;
    int visible = 0;

    if (send_all(fd, &request, sizeof(request)) < 0 ||
        recv_all(fd, &reply, sizeof(reply)) < 0 ||
        reply.magic != CAPTURE_MAGIC || reply.type != CAPTURE_REPLY ||
        reply.protocol_version != CAPTURE_PROTOCOL_VERSION || reply.status ||
        !reply.payload_size || reply.payload_size > 256u * 1024u * 1024u)
        return -1;
    remaining = reply.payload_size;
    while (remaining) {
        size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        size_t index;
        if (recv_all(fd, buffer, chunk) < 0) return -1;
        for (index = 0; !visible && index + 2 < chunk; index += 4)
            visible = buffer[index] || buffer[index + 1] || buffer[index + 2];
        remaining -= (uint32_t)chunk;
    }
    return visible ? 0 : -1;
}

static int test_relative_input(int fd)
{
    const CaptureInputRequest request = {
        CAPTURE_MAGIC, CAPTURE_INPUT_REQUEST, CAPTURE_PROTOCOL_VERSION, 2
    };
    CaptureInputEvent events[2] = {0};
    CaptureInputReply reply;
    events[0].type = CAPTURE_INPUT_MOUSE;
    events[0].dx = 1;
    events[0].flags = 1u;
    events[1].type = CAPTURE_INPUT_MOUSE;
    events[1].dx = -1;
    events[1].flags = 1u;
    if (send_all(fd, &request, sizeof(request)) < 0 ||
        send_all(fd, events, sizeof(events)) < 0 ||
        recv_all(fd, &reply, sizeof(reply)) < 0 ||
        reply.magic != CAPTURE_MAGIC || reply.type != CAPTURE_INPUT_REPLY ||
        reply.protocol_version != CAPTURE_PROTOCOL_VERSION || reply.status ||
        reply.accepted_count != 2)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    struct sockaddr_in address;
    struct timeval timeout = {3, 0};
    char *end = NULL;
    unsigned long port;
    int connection;

    if (argc != 2) return 2;
    port = strtoul(argv[1], &end, 10);
    if (!end || *end || !port || port > 65535) return 2;
    connection = socket(AF_INET, SOCK_STREAM, 0);
    if (connection < 0) return 1;
    setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);
    if (connect(connection, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        authenticate(connection) < 0 ||
        request_frame(connection) < 0) {
        close(connection);
        return 1;
    }
    sleep(6);
    if (request_frame(connection) < 0 || test_relative_input(connection) < 0) {
        fprintf(stderr, "capture connection did not survive idle period\n");
        close(connection);
        return 1;
    }
    close(connection);
    printf("capture connection idle reuse and relative input passed\n");
    return 0;
}
