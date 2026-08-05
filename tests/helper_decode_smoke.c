#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "helper_protocol.h"

static int transfer_all(int fd, void *data, size_t size, int sending)
{
    uint8_t *cursor = data;
    while (size) {
        ssize_t count = sending ? send(fd, cursor, size, MSG_NOSIGNAL) : recv(fd, cursor, size, 0);
        if (count <= 0) {
            if (count < 0 && errno == EINTR) continue;
            return -1;
        }
        cursor += count;
        size -= count;
    }
    return 0;
}

int main(int argc, char **argv)
{
    HelperInitMessage init = {0};
    HelperFrameMessage packet = {0};
    HelperReply reply;
    struct sockaddr_in address = {0};
    FILE *file;
    uint8_t *data;
    long file_size;
    int32_t enabled;
    int fd;
    unsigned long port = 47891;
    if (argc != 2 && argc != 3) return 2;
    if (argc == 3) port = strtoul(argv[2], NULL, 10);
    file = fopen(argv[1], "rb");
    if (!file) return 3;
    fseek(file, 0, SEEK_END);
    file_size = ftell(file);
    rewind(file);
    if (file_size <= 0 || file_size > 128 * 1024 * 1024) return 4;
    data = malloc((size_t)file_size);
    if (!data || fread(data, 1, (size_t)file_size, file) != (size_t)file_size) return 5;
    fclose(file);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) return 6;
    init.magic = HELPER_MAGIC;
    init.type = HELPER_DECODER_INIT;
    if (transfer_all(fd, &init, sizeof(init), 1) < 0 ||
        transfer_all(fd, &enabled, sizeof(enabled), 0) < 0 || !enabled) return 7;
    packet.magic = HELPER_MAGIC;
    packet.type = HELPER_DECODE_PACKET;
    packet.payload_size = (uint32_t)file_size;
    if (transfer_all(fd, &packet, sizeof(packet), 1) < 0 ||
        transfer_all(fd, data, (size_t)file_size, 1) < 0 ||
        transfer_all(fd, &reply, sizeof(reply), 0) < 0) return 8;
    free(data);
    close(fd);
    if (reply.magic != HELPER_MAGIC || reply.type != HELPER_REPLY || reply.status ||
        reply.width != 1920 || reply.height != 1080 ||
        reply.payload_size != reply.width * reply.height * 3 / 2) return 9;
    printf("Decoded Vulkan H.264 frame: %ux%u, %u NV12 bytes\n",
           reply.width, reply.height, reply.payload_size);
    return 0;
}
