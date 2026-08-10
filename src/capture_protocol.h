#ifndef UU_CAPTURE_PROTOCOL_H
#define UU_CAPTURE_PROTOCOL_H

#include <stdint.h>

#define CAPTURE_MAGIC 0x31504355u
#define CAPTURE_REQUEST 1u
#define CAPTURE_REPLY 2u
#define CAPTURE_INPUT_REQUEST 3u
#define CAPTURE_INPUT_REPLY 4u
#define CAPTURE_AUTH_REQUEST 5u
#define CAPTURE_AUTH_REPLY 6u
#define CAPTURE_PROTOCOL_VERSION 2u
#define CAPTURE_DEFAULT_PORT 47892u
#define CAPTURE_MAX_INPUT_EVENTS 64u
#define CAPTURE_AUTH_TOKEN_SIZE 64u

#define CAPTURE_INPUT_MOUSE 0u
#define CAPTURE_INPUT_KEYBOARD 1u

#pragma pack(push, 1)
typedef struct CaptureRequest {
    uint32_t magic;
    uint32_t type;
    uint32_t protocol_version;
    uint32_t reserved;
} CaptureRequest;

typedef struct CaptureReply {
    uint32_t magic;
    uint32_t type;
    uint32_t protocol_version;
    uint32_t status;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t payload_size;
} CaptureReply;

typedef struct CaptureInputRequest {
    uint32_t magic;
    uint32_t type;
    uint32_t protocol_version;
    uint32_t event_count;
} CaptureInputRequest;

typedef struct CaptureInputEvent {
    uint32_t type;
    int32_t dx;
    int32_t dy;
    uint32_t mouse_data;
    uint32_t flags;
    uint16_t virtual_key;
    uint16_t scan_code;
} CaptureInputEvent;

typedef struct CaptureInputReply {
    uint32_t magic;
    uint32_t type;
    uint32_t protocol_version;
    uint32_t status;
    uint32_t accepted_count;
} CaptureInputReply;

typedef struct CaptureAuthRequest {
    uint32_t magic;
    uint32_t type;
    uint32_t protocol_version;
    uint32_t token_size;
    char token[CAPTURE_AUTH_TOKEN_SIZE];
} CaptureAuthRequest;

typedef struct CaptureAuthReply {
    uint32_t magic;
    uint32_t type;
    uint32_t protocol_version;
    uint32_t status;
} CaptureAuthReply;
#pragma pack(pop)

#endif
