#ifndef UU_AMF_HELPER_PROTOCOL_H
#define UU_AMF_HELPER_PROTOCOL_H

#include <stdint.h>

#define HELPER_MAGIC 0x31464d41u
#define HELPER_INIT 1u
#define HELPER_FRAME 2u
#define HELPER_REPLY 3u
#define HELPER_DECODER_INIT 4u
#define HELPER_DECODE_PACKET 5u
#define HELPER_DEFAULT_PORT 47890u

#pragma pack(push, 1)
typedef struct HelperInitMessage {
    uint32_t magic;
    uint32_t type;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t reserved;
    uint64_t bitrate;
    uint32_t fps_num;
    uint32_t fps_den;
} HelperInitMessage;

typedef struct HelperFrameMessage {
    uint32_t magic;
    uint32_t type;
    uint32_t payload_size;
    uint32_t reserved;
    int64_t pts;
    int64_t duration;
} HelperFrameMessage;

typedef struct HelperReply {
    uint32_t magic;
    uint32_t type;
    uint32_t status;
    uint32_t payload_size;
    uint32_t width;
    uint32_t height;
} HelperReply;
#pragma pack(pop)

#endif
