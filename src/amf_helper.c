#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>

#include "helper_protocol.h"

typedef struct EncoderState {
    AVCodecContext *codec;
    AVBufferRef *device;
    AVBufferRef *frames;
    uint32_t width;
    uint32_t height;
    int64_t next_pts;
} EncoderState;

typedef struct DecoderState {
    AVCodecContext *codec;
    AVBufferRef *device;
} DecoderState;

static int recv_all(int fd, void *data, size_t size)
{
    uint8_t *cursor = data;
    while (size) {
        ssize_t count = recv(fd, cursor, size, 0);
        if (count == 0) return 0;
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        cursor += count;
        size -= count;
    }
    return 1;
}

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
        size -= count;
    }
    return 0;
}

static enum AVPixelFormat decoder_get_format(AVCodecContext *codec, const enum AVPixelFormat *formats)
{
    (void)codec;
    while (*formats != AV_PIX_FMT_NONE) {
        if (*formats == AV_PIX_FMT_VULKAN) return *formats;
        ++formats;
    }
    return AV_PIX_FMT_NONE;
}

static void decoder_close(DecoderState *state)
{
    avcodec_free_context(&state->codec);
    av_buffer_unref(&state->device);
    memset(state, 0, sizeof(*state));
}

static int decoder_init(DecoderState *state)
{
    const AVCodec *codec;
    int result;
    decoder_close(state);
    codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return AVERROR_DECODER_NOT_FOUND;
    result = av_hwdevice_ctx_create(&state->device, AV_HWDEVICE_TYPE_VULKAN, NULL, NULL, 0);
    if (result < 0) goto fail;
    state->codec = avcodec_alloc_context3(codec);
    if (!state->codec) { result = AVERROR(ENOMEM); goto fail; }
    state->codec->hw_device_ctx = av_buffer_ref(state->device);
    state->codec->get_format = decoder_get_format;
    state->codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    result = avcodec_open2(state->codec, codec, NULL);
    if (result < 0) goto fail;
    fprintf(stderr, "Vulkan H.264 decoder initialized\n");
    return 0;
fail:
    decoder_close(state);
    return result;
}

static int decoder_packet(DecoderState *state, const uint8_t *data, uint32_t size,
                          uint8_t **frame_data, uint32_t *frame_size,
                          uint32_t *width, uint32_t *height)
{
    AVPacket *packet = NULL;
    AVFrame *gpu = NULL;
    AVFrame *host = NULL;
    uint8_t *output = NULL;
    uint64_t output_size;
    int y;
    int result;
    *frame_data = NULL;
    *frame_size = 0;
    *width = 0;
    *height = 0;
    packet = av_packet_alloc();
    gpu = av_frame_alloc();
    host = av_frame_alloc();
    if (!packet || !gpu || !host) { result = AVERROR(ENOMEM); goto done; }
    result = av_new_packet(packet, size);
    if (result < 0) goto done;
    memcpy(packet->data, data, size);
    result = avcodec_send_packet(state->codec, packet);
    if (result < 0) goto done;
    result = avcodec_receive_frame(state->codec, gpu);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) { result = 0; goto done; }
    if (result < 0) goto done;
    if (gpu->format != AV_PIX_FMT_VULKAN) { result = AVERROR(ENOSYS); goto done; }
    result = av_hwframe_transfer_data(host, gpu, 0);
    if (result < 0) goto done;
    if (host->width <= 0 || host->height <= 0 || (host->width & 1) || (host->height & 1)) {
        result = AVERROR(EINVAL);
        goto done;
    }
    output_size = (uint64_t)host->width * host->height * 3 / 2;
    if (output_size > UINT32_MAX) { result = AVERROR(EOVERFLOW); goto done; }
    output = malloc((size_t)output_size);
    if (!output) { result = AVERROR(ENOMEM); goto done; }
    for (y = 0; y < host->height; ++y)
        memcpy(output + (size_t)y * host->width, host->data[0] + (size_t)y * host->linesize[0], host->width);
    if (host->format == AV_PIX_FMT_NV12) {
        for (y = 0; y < host->height / 2; ++y)
            memcpy(output + (size_t)host->width * host->height + (size_t)y * host->width,
                   host->data[1] + (size_t)y * host->linesize[1], host->width);
    } else if (host->format == AV_PIX_FMT_YUV420P) {
        for (y = 0; y < host->height / 2; ++y) {
            int x;
            uint8_t *row = output + (size_t)host->width * host->height + (size_t)y * host->width;
            for (x = 0; x < host->width / 2; ++x) {
                row[x * 2] = host->data[1][(size_t)y * host->linesize[1] + x];
                row[x * 2 + 1] = host->data[2][(size_t)y * host->linesize[2] + x];
            }
        }
    } else {
        result = AVERROR(ENOSYS);
        goto done;
    }
    *frame_data = output;
    *frame_size = (uint32_t)output_size;
    *width = host->width;
    *height = host->height;
    output = NULL;
    result = 0;
done:
    free(output);
    av_frame_free(&host);
    av_frame_free(&gpu);
    av_packet_free(&packet);
    return result;
}

static void encoder_close(EncoderState *state)
{
    avcodec_free_context(&state->codec);
    av_buffer_unref(&state->frames);
    av_buffer_unref(&state->device);
    memset(state, 0, sizeof(*state));
}

static int encoder_init(EncoderState *state, const HelperInitMessage *message)
{
    const AVCodec *codec;
    AVBufferRef *frames_ref;
    AVHWFramesContext *frames;
    int result;
    encoder_close(state);
    codec = avcodec_find_encoder_by_name("h264_vulkan");
    if (!codec) return AVERROR_ENCODER_NOT_FOUND;
    result = av_hwdevice_ctx_create(&state->device, AV_HWDEVICE_TYPE_VULKAN, NULL, NULL, 0);
    if (result < 0) goto fail;
    frames_ref = av_hwframe_ctx_alloc(state->device);
    if (!frames_ref) { result = AVERROR(ENOMEM); goto fail; }
    frames = (AVHWFramesContext *)frames_ref->data;
    frames->format = AV_PIX_FMT_VULKAN;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = message->width;
    frames->height = message->height;
    frames->initial_pool_size = 8;
    result = av_hwframe_ctx_init(frames_ref);
    if (result < 0) { av_buffer_unref(&frames_ref); goto fail; }
    state->frames = frames_ref;
    state->codec = avcodec_alloc_context3(codec);
    if (!state->codec) { result = AVERROR(ENOMEM); goto fail; }
    state->codec->width = message->width;
    state->codec->height = message->height;
    state->codec->pix_fmt = AV_PIX_FMT_VULKAN;
    state->codec->hw_frames_ctx = av_buffer_ref(state->frames);
    state->codec->time_base = (AVRational){message->fps_den ? (int)message->fps_den : 1, message->fps_num ? (int)message->fps_num : 30};
    state->codec->framerate = (AVRational){message->fps_num ? (int)message->fps_num : 30, message->fps_den ? (int)message->fps_den : 1};
    state->codec->bit_rate = message->bitrate ? message->bitrate : 8000000;
    state->codec->rc_max_rate = state->codec->bit_rate;
    state->codec->rc_buffer_size = state->codec->bit_rate;
    state->codec->gop_size = 60;
    state->codec->max_b_frames = 0;
    state->codec->profile = AV_PROFILE_H264_HIGH;
    state->codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    av_opt_set_int(state->codec->priv_data, "quality", 0, 0);
    av_opt_set_int(state->codec->priv_data, "async_depth", 1, 0);
    av_opt_set_int(state->codec->priv_data, "tune", 3, 0);
    av_opt_set(state->codec->priv_data, "usage", "stream", 0);
    av_opt_set(state->codec->priv_data, "content", "desktop", 0);
    av_opt_set_int(state->codec->priv_data, "rc_mode", 2, 0);
    result = avcodec_open2(state->codec, codec, NULL);
    if (result < 0) goto fail;
    state->width = message->width;
    state->height = message->height;
    return 0;
fail:
    encoder_close(state);
    return result;
}

static int encoder_frame(EncoderState *state, const uint8_t *data, const HelperFrameMessage *message, uint8_t **packet_data, uint32_t *packet_size)
{
    AVFrame *host = NULL;
    AVFrame *gpu = NULL;
    AVPacket *packet = NULL;
    uint8_t *output = NULL;
    size_t output_size = 0;
    int y;
    int result = 0;
    *packet_data = NULL;
    *packet_size = 0;
    if (!state->codec || message->payload_size != state->width * state->height * 3 / 2) return AVERROR(EINVAL);
    host = av_frame_alloc();
    gpu = av_frame_alloc();
    packet = av_packet_alloc();
    if (!host || !gpu || !packet) { result = AVERROR(ENOMEM); goto done; }
    host->format = AV_PIX_FMT_NV12;
    host->width = state->width;
    host->height = state->height;
    result = av_frame_get_buffer(host, 32);
    if (result < 0) goto done;
    for (y = 0; y < (int)state->height; ++y) memcpy(host->data[0] + y * host->linesize[0], data + y * state->width, state->width);
    for (y = 0; y < (int)state->height / 2; ++y) memcpy(host->data[1] + y * host->linesize[1], data + state->width * state->height + y * state->width, state->width);
    result = av_hwframe_get_buffer(state->frames, gpu, 0);
    if (result < 0) goto done;
    result = av_hwframe_transfer_data(gpu, host, 0);
    if (result < 0) goto done;
    gpu->pts = state->next_pts++;
    result = avcodec_send_frame(state->codec, gpu);
    if (result < 0) goto done;
    for (;;) {
        uint8_t *larger;
        result = avcodec_receive_packet(state->codec, packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) { result = 0; break; }
        if (result < 0) goto done;
        larger = realloc(output, output_size + packet->size);
        if (!larger) { result = AVERROR(ENOMEM); goto done; }
        output = larger;
        memcpy(output + output_size, packet->data, packet->size);
        output_size += packet->size;
        av_packet_unref(packet);
    }
    *packet_data = output;
    *packet_size = output_size;
    output = NULL;
done:
    free(output);
    av_packet_free(&packet);
    av_frame_free(&gpu);
    av_frame_free(&host);
    return result;
}

static void handle_client(int client)
{
    HelperInitMessage init_message;
    EncoderState encoder = {0};
    DecoderState decoder = {0};
    HelperInitReply init_reply = {0, HELPER_PROTOCOL_VERSION};
    int receive_result;
    receive_result = recv_all(client, &init_message, sizeof(init_message));
    if (receive_result <= 0 || init_message.magic != HELPER_MAGIC ||
        init_message.reserved != HELPER_PROTOCOL_VERSION ||
        (init_message.type != HELPER_INIT && init_message.type != HELPER_DECODER_INIT)) return;
    init_reply.enabled = init_message.type == HELPER_INIT
        ? encoder_init(&encoder, &init_message) == 0
        : decoder_init(&decoder) == 0;
    if (send_all(client, &init_reply, sizeof(init_reply)) < 0 || !init_reply.enabled) goto done;
    for (;;) {
        HelperFrameMessage frame_message;
        HelperReply reply = {HELPER_MAGIC, HELPER_REPLY, 0, 0, 0, 0};
        uint8_t *frame_data = NULL;
        uint8_t *packet_data = NULL;
        uint32_t packet_size = 0;
        int result;
        receive_result = recv_all(client, &frame_message, sizeof(frame_message));
        if (receive_result <= 0) break;
        if (frame_message.magic != HELPER_MAGIC ||
            (frame_message.type != HELPER_FRAME && frame_message.type != HELPER_DECODE_PACKET) ||
            frame_message.payload_size > 128u * 1024u * 1024u) break;
        frame_data = malloc(frame_message.payload_size);
        if (!frame_data || recv_all(client, frame_data, frame_message.payload_size) <= 0) { free(frame_data); break; }
        if (frame_message.type == HELPER_FRAME) {
            result = encoder_frame(&encoder, frame_data, &frame_message, &packet_data, &packet_size);
        } else {
            result = decoder_packet(&decoder, frame_data, frame_message.payload_size,
                                    &packet_data, &packet_size, &reply.width, &reply.height);
        }
        free(frame_data);
        reply.status = result < 0 ? (uint32_t)(-result) : 0;
        reply.payload_size = packet_size;
        if (send_all(client, &reply, sizeof(reply)) < 0 || (packet_size && send_all(client, packet_data, packet_size) < 0)) { free(packet_data); break; }
        free(packet_data);
    }
done:
    encoder_close(&encoder);
    decoder_close(&decoder);
}

static void *client_thread(void *opaque)
{
    int client = (int)(intptr_t)opaque;
    handle_client(client);
    close(client);
    return NULL;
}

int main(int argc, char **argv)
{
    int server;
    int reuse = 1;
    unsigned long port = HELPER_DEFAULT_PORT;
    struct sockaddr_in address;
    if (argc == 2) {
        char *end = NULL;
        port = strtoul(argv[1], &end, 10);
        if (!end || *end || port == 0 || port > 65535) {
            fprintf(stderr, "invalid port\n");
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { perror("socket"); return 1; }
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(server, 4) < 0) { perror("listen"); close(server); return 1; }
    fprintf(stderr, "uu-amf-helper listening on 127.0.0.1:%lu\n", port);
    for (;;) {
        int client = accept(server, NULL, NULL);
        pthread_t thread;
        struct timeval timeout = {5, 0};
        if (client < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (pthread_create(&thread, NULL, client_thread, (void *)(intptr_t)client) != 0) {
            close(client);
            continue;
        }
        pthread_detach(thread);
    }
    close(server);
    return 1;
}
