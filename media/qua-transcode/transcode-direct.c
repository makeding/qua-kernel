/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* RTD1295 MPEG-2 -> H.264: feed hardware decoder frames directly to encoder. */
#include <OMX_Core.h>
#include <OMX_Component.h>
#include <OMX_Index.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define INIT(x) do { memset(&(x), 0, sizeof(x)); (x).nSize = sizeof(x); \
    (x).nVersion.s.nVersionMajor = 1; (x).nVersion.s.nVersionMinor = 1; \
    (x).nVersion.s.nRevision = 2; } while (0)
#define MAX_BUFFERS 64

static OMX_HANDLETYPE decoder, encoder;
static OMX_BUFFERHEADERTYPE *dec_in[MAX_BUFFERS], *dec_out[MAX_BUFFERS];
static OMX_BUFFERHEADERTYPE *enc_in[MAX_BUFFERS], *enc_out[MAX_BUFFERS];
static _Atomic int dec_idle, dec_exec, enc_idle, enc_exec, failed;
static _Atomic int changed, disabled, enabled, decoder_eos, encoder_eos;
static _Atomic int input_free[MAX_BUFFERS], output_free[MAX_BUFFERS], encoder_input_free[MAX_BUFFERS];
static _Atomic int encoded_free[MAX_BUFFERS];
static _Atomic unsigned decoded_frames, encoded_frames;
static uint8_t *encoded_partial;
static int (*gpu_bob_init)(unsigned, unsigned, unsigned, unsigned);
static int (*gpu_bob_frame)(void *, const void *, size_t);
static int (*gpu_bob_register)(unsigned, int, int);
static int (*gpu_bob_dmabuf)(unsigned);
static void (*gpu_bob_close)(void);
static void *gpu_bob_library;
static int gpu_bob_direct;
static size_t gpu_bob_size;

static int buffer_fd(const void *address)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[1024]; unsigned long lo, hi, offset, inode; unsigned dev_major, dev_minor;
    int result = -1;
    if (!maps) return -1;
    while (fgets(line, sizeof(line), maps)) {
        if (sscanf(line, "%lx-%lx %*s %lx %x:%x %lu", &lo, &hi, &offset,
                   &dev_major, &dev_minor, &inode) == 6 &&
            (uintptr_t)address == lo && offset == 0 && inode && strstr(line, "/dmabuf:")) {
            for (int fd = 0; fd < 1024; fd++) {
                struct stat st;
                if (!fstat(fd, &st) && (unsigned long)st.st_ino == inode &&
                    major(st.st_dev) == dev_major && minor(st.st_dev) == dev_minor) { result = fd; break; }
            }
            break;
        }
    }
    fclose(maps);
    return result;
}

static void describe_buffer(const char *name, unsigned index, const void *address)
{
    if (!getenv("QUA_BUFFER_DIAG")) return;
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[1024]; unsigned long lo, hi, offset, inode;
    if (!maps) return;
    while (fgets(line, sizeof(line), maps)) {
        if (sscanf(line, "%lx-%lx %*s %lx %*s %lu", &lo, &hi, &offset, &inode) == 4 &&
            (uintptr_t)address >= lo && (uintptr_t)address < hi) {
            fprintf(stderr, "BUFFER_MAP %s[%u] addr=%p map=%s", name, index, address, line);
            for (int fd = 0; fd < 1024; fd++) {
                struct stat st;
                if (!fstat(fd, &st) && inode && (unsigned long)st.st_ino == inode)
                    fprintf(stderr, "BUFFER_FD %s[%u] fd=%d inode=%lu\n", name, index, fd, inode);
            }
            break;
        }
    }
    fclose(maps);
}
static size_t encoded_partial_size;
static AVFormatContext *mux;
static AVStream *video_out, *audio_out;
static pthread_mutex_t mux_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pending_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned pending_queue[MAX_BUFFERS], pending_head, pending_count;
static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t input_space = PTHREAD_COND_INITIALIZER;
static AVPacket *input_queue[256];
static unsigned input_head, input_count;
static _Atomic int reader_finished, reader_stop;
static AVFormatContext *reader_input;
static int reader_video_index;
static int64_t reader_first_pts = AV_NOPTS_VALUE, reader_last_pts = AV_NOPTS_VALUE;
static struct timespec reader_first_time, reader_last_time;
static int reader_sample_complete;
static int64_t duration_us = 10000000;

static void *read_packets(void *unused)
{
    (void)unused;
    while (!reader_stop && !failed) {
        AVPacket *packet = av_packet_alloc();
        if (!packet) { failed = 1; break; }
        if (av_read_frame(reader_input, packet) < 0) { av_packet_free(&packet); break; }
        pthread_mutex_lock(&input_mutex);
        if (!reader_sample_complete && packet->stream_index == reader_video_index && packet->pts != AV_NOPTS_VALUE) {
            if (reader_first_pts == AV_NOPTS_VALUE && (packet->flags & AV_PKT_FLAG_KEY)) {
                reader_first_pts = packet->pts;
                clock_gettime(CLOCK_MONOTONIC, &reader_first_time);
            }
            if (reader_first_pts != AV_NOPTS_VALUE) {
                reader_last_pts = packet->pts;
                clock_gettime(CLOCK_MONOTONIC, &reader_last_time);
                if (av_rescale_q(reader_last_pts - reader_first_pts,
                                 reader_input->streams[reader_video_index]->time_base,
                                 (AVRational){1, 1000000}) >= duration_us)
                    reader_sample_complete = 1;
            }
        }
        while (input_count == 256 && !reader_stop && !failed)
            pthread_cond_wait(&input_space, &input_mutex);
        if (reader_stop || failed) {
            pthread_mutex_unlock(&input_mutex);
            av_packet_free(&packet);
            break;
        }
        input_queue[(input_head + input_count) % 256] = packet;
        input_count++;
        pthread_mutex_unlock(&input_mutex);
    }
    reader_finished = 1;
    return NULL;
}

static int pop_packet(AVPacket *packet)
{
    pthread_mutex_lock(&input_mutex);
    if (!input_count) {
        int done = reader_finished;
        pthread_mutex_unlock(&input_mutex);
        return done ? -1 : 0;
    }
    AVPacket *queued = input_queue[input_head];
    input_head = (input_head + 1) % 256;
    input_count--;
    pthread_cond_signal(&input_space);
    pthread_mutex_unlock(&input_mutex);
    av_packet_unref(packet);
    av_packet_move_ref(packet, queued);
    av_packet_free(&queued);
    return 1;
}

static OMX_ERRORTYPE event_cb(OMX_HANDLETYPE h, OMX_PTR app, OMX_EVENTTYPE event,
                              OMX_U32 a, OMX_U32 b, OMX_PTR data)
{
    (void)app; (void)data;
    fprintf(stderr, "OMX_EVENT %c event=%u a=%#x b=%#x\n",
            h == decoder ? 'D' : 'E', event, a, b);
    if (event == OMX_EventError) failed = 1;
    if (event == OMX_EventCmdComplete && a == OMX_CommandStateSet) {
        if (h == decoder && b == OMX_StateIdle) dec_idle = 1;
        if (h == decoder && b == OMX_StateExecuting) dec_exec = 1;
        if (h == encoder && b == OMX_StateIdle) enc_idle = 1;
        if (h == encoder && b == OMX_StateExecuting) enc_exec = 1;
    }
    if (h == decoder && event == OMX_EventPortSettingsChanged && a == 1) changed = 1;
    if (h == decoder && event == OMX_EventCmdComplete && a == OMX_CommandPortDisable && b == 1) disabled = 1;
    if (h == decoder && event == OMX_EventCmdComplete && a == OMX_CommandPortEnable && b == 1) enabled = 1;
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE empty_cb(OMX_HANDLETYPE h, OMX_PTR app, OMX_BUFFERHEADERTYPE *b)
{
    uintptr_t slot = (uintptr_t)b->pAppPrivate;
    (void)app;
    if (slot < 1 || slot > MAX_BUFFERS) { failed = 1; return OMX_ErrorNone; }
    if (h == decoder) input_free[slot - 1] = 1;
    else if (h == encoder) encoder_input_free[slot - 1] = 1;
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE fill_cb(OMX_HANDLETYPE h, OMX_PTR app, OMX_BUFFERHEADERTYPE *b)
{
    uintptr_t slot = (uintptr_t)b->pAppPrivate;
    (void)app;
    if (slot < 1 || slot > MAX_BUFFERS) { failed = 1; return OMX_ErrorNone; }
    slot--;
    if (h == decoder) {
        if (b->nFlags & OMX_BUFFERFLAG_EOS) decoder_eos = 1;
        if (b->nFilledLen || (b->nFlags & OMX_BUFFERFLAG_EOS)) {
            pthread_mutex_lock(&pending_mutex);
            if (pending_count == MAX_BUFFERS) failed = 1;
            else {
                pending_queue[(pending_head + pending_count) % MAX_BUFFERS] = slot;
                pending_count++;
            }
            pthread_mutex_unlock(&pending_mutex);
            if (b->nFilledLen) decoded_frames++;
        } else {
            output_free[slot] = 1;
        }
    } else if (h == encoder) {
        if (b->nFilledLen) {
            if (b->nOffset > b->nAllocLen || b->nFilledLen > b->nAllocLen - b->nOffset ||
                encoded_partial_size + b->nFilledLen > 16 * 1024 * 1024) {
                failed = 1;
                return OMX_ErrorNone;
            }
            uint8_t *grown = av_realloc(encoded_partial, encoded_partial_size + b->nFilledLen);
            if (!grown) { failed = 1; return OMX_ErrorNone; }
            encoded_partial = grown;
            memcpy(encoded_partial + encoded_partial_size, b->pBuffer + b->nOffset, b->nFilledLen);
            encoded_partial_size += b->nFilledLen;
        }
        if ((b->nFlags & OMX_BUFFERFLAG_ENDOFFRAME) && encoded_partial_size) {
            AVPacket *pkt = av_packet_alloc();
            if (!pkt || av_new_packet(pkt, encoded_partial_size) < 0) {
                if (pkt) av_packet_free(&pkt);
                failed = 1;
                return OMX_ErrorNone;
            }
            memcpy(pkt->data, encoded_partial, encoded_partial_size);
            encoded_partial_size = 0;
            pkt->stream_index = video_out->index;
            pkt->pts = av_rescale_q(b->nTimeStamp, (AVRational){1, 1000000}, video_out->time_base);
            pkt->dts = pkt->pts;
            if (b->nFlags & OMX_BUFFERFLAG_SYNCFRAME) pkt->flags |= AV_PKT_FLAG_KEY;
            pthread_mutex_lock(&mux_mutex);
            int ret = av_interleaved_write_frame(mux, pkt);
            pthread_mutex_unlock(&mux_mutex);
            av_packet_free(&pkt);
            if (ret < 0) { fprintf(stderr, "ENC_MUX_ERROR=%d\n", ret); failed = 1; }
            encoded_frames++;
        }
        if (b->nFlags & OMX_BUFFERFLAG_EOS) encoder_eos = 1;
        encoded_free[slot] = 1;
    }
    return OMX_ErrorNone;
}

static int wait_flag(_Atomic int *flag)
{
    for (unsigned i = 0; i < 1000 && !*flag && !failed; i++) usleep(10000);
    return *flag && !failed ? 0 : -1;
}

static int setup_encoder(OMX_PARAM_PORTDEFINITIONTYPE *d, AVRational rate)
{
    OMX_PARAM_PORTDEFINITIONTYPE e[2];
    OMX_VIDEO_PARAM_BITRATETYPE bitrate;
    OMX_VIDEO_PARAM_AVCTYPE avc;
    OMX_CALLBACKTYPE callbacks = {event_cb, empty_cb, fill_cb};
    OMX_ERRORTYPE ret = OMX_GetHandle(&encoder, "OMX.realtek.video.enc", NULL, &callbacks);
    if (ret) { fprintf(stderr, "ENC_HANDLE=%#x\n", ret); return -1; }
    for (unsigned i = 0; i < 2; i++) {
        INIT(e[i]); e[i].nPortIndex = i;
        if (OMX_GetParameter(encoder, OMX_IndexParamPortDefinition, &e[i])) return -1;
    }
    e[0].format.video.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
    e[0].format.video.nFrameWidth = d->format.video.nFrameWidth;
    e[0].format.video.nFrameHeight = d->format.video.nFrameHeight;
    e[0].format.video.nStride = d->format.video.nStride;
    e[0].format.video.nSliceHeight = d->format.video.nSliceHeight;
    e[0].format.video.xFramerate = (OMX_U32)(av_q2d(rate) * 65536.0);
    e[0].nBufferCountActual = d->nBufferCountActual;
    if (e[0].nBufferSize < d->nBufferSize) e[0].nBufferSize = d->nBufferSize;
    ret = OMX_SetParameter(encoder, OMX_IndexParamPortDefinition, &e[0]);
    fprintf(stderr, "ENC_INPUT_SET=%#x buffers=%u\n", ret, e[0].nBufferCountActual);
    if (ret) return -1;
    e[1].format.video.nFrameWidth = d->format.video.nFrameWidth;
    e[1].format.video.nFrameHeight = d->format.video.nFrameHeight;
    e[1].format.video.nBitrate = 3000000;
    e[1].format.video.xFramerate = e[0].format.video.xFramerate;
    e[1].format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;
    ret = OMX_SetParameter(encoder, OMX_IndexParamPortDefinition, &e[1]);
    if (ret) { fprintf(stderr, "ENC_OUTPUT_SET=%#x\n", ret); return -1; }
    for (unsigned i = 0; i < 2; i++) {
        INIT(e[i]); e[i].nPortIndex = i;
        if (OMX_GetParameter(encoder, OMX_IndexParamPortDefinition, &e[i])) return -1;
    }
    if (e[0].nBufferCountActual != d->nBufferCountActual || e[1].nBufferCountActual > MAX_BUFFERS)
        return -1;
    INIT(bitrate); bitrate.nPortIndex = 1; bitrate.eControlRate = OMX_Video_ControlRateVariable;
    bitrate.nTargetBitrate = 3000000;
    OMX_SetParameter(encoder, OMX_IndexParamVideoBitrate, &bitrate);
    INIT(avc); avc.nPortIndex = 1;
    if (!OMX_GetParameter(encoder, OMX_IndexParamVideoAvc, &avc)) {
        avc.nBFrames = 0;
        avc.nPFrames = 59;
        ret = OMX_SetParameter(encoder, OMX_IndexParamVideoAvc, &avc);
        if (ret) { fprintf(stderr, "ENC_AVC_SET=%#x\n", ret); return -1; }
    }
    if (OMX_SendCommand(encoder, OMX_CommandStateSet, OMX_StateIdle, NULL)) return -1;
    for (unsigned i = 0; i < e[0].nBufferCountActual; i++) {
        ret = OMX_AllocateBuffer(encoder, &enc_in[i], 0, (OMX_PTR)(uintptr_t)(i + 1), e[0].nBufferSize);
        if (ret || !enc_in[i]->pBuffer || enc_in[i]->nAllocLen < d->nBufferSize) {
            fprintf(stderr, "ENC_ALLOC_INPUT %u=%#x\n", i, ret); return -1;
        }
        encoder_input_free[i] = 1;
        describe_buffer("enc_in", i, enc_in[i]->pBuffer);
    }
    for (unsigned i = 0; i < e[1].nBufferCountActual; i++) {
        ret = OMX_AllocateBuffer(encoder, &enc_out[i], 1, (OMX_PTR)(uintptr_t)(i + 1), e[1].nBufferSize);
        if (ret) return -1;
    }
    if (wait_flag(&enc_idle) || OMX_SendCommand(encoder, OMX_CommandStateSet, OMX_StateExecuting, NULL) ||
        wait_flag(&enc_exec)) return -1;
    for (unsigned i = 0; i < e[1].nBufferCountActual; i++)
        if (OMX_FillThisBuffer(encoder, enc_out[i])) return -1;
    fprintf(stderr, "ENC_READY inputs=%u out=%u stride=%d slice=%u\n", e[0].nBufferCountActual,
            e[1].nBufferCountActual, e[0].format.video.nStride, e[0].format.video.nSliceHeight);
    return e[1].nBufferCountActual;
}

int main(int argc, char **argv)
{
    OMX_CALLBACKTYPE callbacks = {event_cb, empty_cb, fill_cb};
    OMX_PARAM_PORTDEFINITIONTYPE defs[2];
    OMX_ERRORTYPE ret;
    AVFormatContext *input = NULL;
    AVDictionary *options = NULL;
    AVPacket *pkt = NULL;
    int video_index, audio_index, configured = 0, enc_count = 0, sent_eos = 0, seen_key = 0;
    int64_t first_pts = AV_NOPTS_VALUE;
    AVRational rate;
    struct timespec encode_start = {0}, encode_end;
    pthread_t reader;
    if (argc != 3) { fprintf(stderr, "usage: %s input-url output.ts\n", argv[0]); return 2; }
    const char *duration = getenv("QUA_DURATION");
    if (duration && *duration) {
        char *end;
        long seconds = strtol(duration, &end, 10);
        if (*end || seconds < 1 || seconds > 3600) {
            fprintf(stderr, "QUA_DURATION must be 1..3600 seconds\n"); return 2;
        }
        duration_us = (int64_t)seconds * 1000000;
    }
    av_log_set_level(AV_LOG_ERROR);
    avformat_network_init();
    av_dict_set(&options, "probesize", "1048576", 0);
    av_dict_set(&options, "analyzeduration", "3000000", 0);
    if (avformat_open_input(&input, argv[1], NULL, &options) < 0 || avformat_find_stream_info(input, NULL) < 0) return 3;
    av_dict_free(&options);
    video_index = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    audio_index = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (video_index < 0 || audio_index < 0 || input->streams[video_index]->codecpar->codec_id != AV_CODEC_ID_MPEG2VIDEO ||
        input->streams[audio_index]->codecpar->codec_id != AV_CODEC_ID_AAC) return 4;
    rate = av_guess_frame_rate(input, input->streams[video_index], NULL);
    if (!rate.num || !rate.den) rate = (AVRational){30000, 1001};
    if (avformat_alloc_output_context2(&mux, NULL, "mpegts", argv[2]) < 0 || !mux) return 5;
    video_out = avformat_new_stream(mux, NULL); audio_out = avformat_new_stream(mux, NULL);
    if (!video_out || !audio_out) return 5;
    video_out->time_base = (AVRational){1, 90000}; video_out->avg_frame_rate = rate;
    video_out->sample_aspect_ratio = input->streams[video_index]->sample_aspect_ratio;
    video_out->codecpar->codec_type = AVMEDIA_TYPE_VIDEO; video_out->codecpar->codec_id = AV_CODEC_ID_H264;
    video_out->codecpar->width = input->streams[video_index]->codecpar->width;
    video_out->codecpar->height = input->streams[video_index]->codecpar->height;
    if (avcodec_parameters_copy(audio_out->codecpar, input->streams[audio_index]->codecpar) < 0) return 5;
    audio_out->codecpar->codec_tag = 0; audio_out->time_base = input->streams[audio_index]->time_base;
    if (avio_open(&mux->pb, argv[2], AVIO_FLAG_WRITE) < 0 || avformat_write_header(mux, NULL) < 0) return 5;
    reader_input = input;
    reader_video_index = video_index;
    if (pthread_create(&reader, NULL, read_packets, NULL)) return 5;
    pkt = av_packet_alloc(); if (!pkt || OMX_Init()) return 6;
    if (OMX_GetHandle(&decoder, "OMX.realtek.video.dec.mpeg2", NULL, &callbacks)) return 6;
    for (unsigned i = 0; i < 2; i++) {
        INIT(defs[i]); defs[i].nPortIndex = i;
        if (OMX_GetParameter(decoder, OMX_IndexParamPortDefinition, &defs[i]) || defs[i].nBufferCountActual > MAX_BUFFERS) return 7;
    }
    defs[1].format.video.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
    if (OMX_SetParameter(decoder, OMX_IndexParamPortDefinition, &defs[1]) ||
        OMX_SendCommand(decoder, OMX_CommandStateSet, OMX_StateIdle, NULL)) return 8;
    for (unsigned i = 0; i < defs[0].nBufferCountActual; i++) {
        if (OMX_AllocateBuffer(decoder, &dec_in[i], 0, (OMX_PTR)(uintptr_t)(i + 1), defs[0].nBufferSize)) return 8;
        input_free[i] = 1;
    }
    for (unsigned i = 0; i < defs[1].nBufferCountActual; i++)
        if (OMX_AllocateBuffer(decoder, &dec_out[i], 1, (OMX_PTR)(uintptr_t)(i + 1), defs[1].nBufferSize)) return 8;
    if (wait_flag(&dec_idle) || OMX_SendCommand(decoder, OMX_CommandStateSet, OMX_StateExecuting, NULL) || wait_flag(&dec_exec)) return 9;
    for (unsigned i = 0; i < defs[1].nBufferCountActual; i++)
        if (OMX_FillThisBuffer(decoder, dec_out[i])) return 9;
    fprintf(stderr, "INPUT video=%ux%u audio=%d rate=%d/%d\n", video_out->codecpar->width,
            video_out->codecpar->height, audio_index, rate.num, rate.den);

    for (unsigned tick = 0; tick < 60000 && !failed && !encoder_eos; tick++) {
        if (changed && !configured) {
            if (OMX_SendCommand(decoder, OMX_CommandPortDisable, 1, NULL)) return 10;
            for (unsigned n = 0; n < 1000; n++) {
                unsigned left = 0;
                for (unsigned i = 0; i < defs[1].nBufferCountActual; i++) if (dec_out[i]) {
                    if (output_free[i]) { OMX_FreeBuffer(decoder, 1, dec_out[i]); dec_out[i] = NULL; }
                    else left++;
                }
                if (!left && disabled) break;
                usleep(10000);
            }
            if (!disabled) return 11;
            INIT(defs[1]); defs[1].nPortIndex = 1;
            if (OMX_GetParameter(decoder, OMX_IndexParamPortDefinition, &defs[1]) || defs[1].nBufferCountActual > MAX_BUFFERS) return 11;
            const char *deint = getenv("QUA_DEINTERLACE");
            if (deint && *deint) {
                unsigned field;
                if (!strcmp(deint, "bob-tff")) field = 0;
                else if (!strcmp(deint, "bob-bff")) field = 1;
                else { fprintf(stderr, "Unknown QUA_DEINTERLACE: %s\n", deint); return 11; }
                gpu_bob_library = dlopen("/opt/qua-video/openmax/gpu-bob.so", RTLD_NOW | RTLD_LOCAL);
                if (!gpu_bob_library) { fprintf(stderr, "GPU_BOB dlopen: %s\n", dlerror()); return 11; }
                gpu_bob_init = dlsym(gpu_bob_library, "qua_gpu_bob_init");
                gpu_bob_frame = dlsym(gpu_bob_library, "qua_gpu_bob_frame");
                gpu_bob_register = dlsym(gpu_bob_library, "qua_gpu_bob_register");
                gpu_bob_dmabuf = dlsym(gpu_bob_library, "qua_gpu_bob_dmabuf");
                gpu_bob_close = dlsym(gpu_bob_library, "qua_gpu_bob_close");
                gpu_bob_direct = getenv("QUA_GPU_IO") && !strcmp(getenv("QUA_GPU_IO"), "dmabuf");
                gpu_bob_size = (size_t)defs[1].format.video.nStride * defs[1].format.video.nSliceHeight * 3 / 2;
                if (!gpu_bob_init || !gpu_bob_frame || !gpu_bob_close ||
                    (gpu_bob_direct && (!gpu_bob_register || !gpu_bob_dmabuf)) ||
                    gpu_bob_init(defs[1].format.video.nStride,
                                 defs[1].format.video.nSliceHeight,
                                 defs[1].format.video.nFrameHeight, field)) {
                    fprintf(stderr, "GPU_BOB initialization failed\n"); return 11;
                }
            }
            enc_count = setup_encoder(&defs[1], rate);
            if (enc_count < 0) return 13;
            if (OMX_SendCommand(decoder, OMX_CommandPortEnable, 1, NULL)) return 12;
            for (unsigned i = 0; i < defs[1].nBufferCountActual; i++) {
                output_free[i] = 0;
                ret = OMX_AllocateBuffer(decoder, &dec_out[i], 1, (OMX_PTR)(uintptr_t)(i + 1),
                                         defs[1].nBufferSize);
                if (ret) { fprintf(stderr, "DEC_ALLOC_OUTPUT %u=%#x\n", i, ret); return 12; }
                describe_buffer("dec_out", i, dec_out[i]->pBuffer);
                if (gpu_bob_direct) {
                    int in_fd = buffer_fd(dec_out[i]->pBuffer);
                    int out_fd = buffer_fd(enc_in[i]->pBuffer);
                    if (gpu_bob_register(i, in_fd, out_fd)) {
                        fprintf(stderr, "GPU_BOB buffer registration slot=%u in=%d out=%d\n", i, in_fd, out_fd);
                        return 12;
                    }
                }
            }
            if (wait_flag(&enabled)) return 12;
            for (unsigned i = 0; i < defs[1].nBufferCountActual; i++)
                if (OMX_FillThisBuffer(decoder, dec_out[i])) return 13;
            configured = 1;
        }
        if (configured) {
            for (;;) {
                unsigned i;
                pthread_mutex_lock(&pending_mutex);
                if (!pending_count) { pthread_mutex_unlock(&pending_mutex); break; }
                i = pending_queue[pending_head];
                if (!encoder_input_free[i]) { pthread_mutex_unlock(&pending_mutex); break; }
                pending_head = (pending_head + 1) % MAX_BUFFERS;
                pending_count--;
                pthread_mutex_unlock(&pending_mutex);
                OMX_BUFFERHEADERTYPE *d = dec_out[i], *e = enc_in[i];
                if (d->nOffset > d->nAllocLen || d->nFilledLen > d->nAllocLen - d->nOffset ||
                    d->nFilledLen > e->nAllocLen) return 14;
                if (gpu_bob_frame && d->nFilledLen) {
                    if ((gpu_bob_direct && (d->nOffset || d->nFilledLen != gpu_bob_size || gpu_bob_dmabuf(i))) ||
                        (!gpu_bob_direct && gpu_bob_frame(e->pBuffer, d->pBuffer + d->nOffset, d->nFilledLen))) {
                        fprintf(stderr, "GPU_BOB frame failed\n"); return 14;
                    }
                } else {
                    memcpy(e->pBuffer, d->pBuffer + d->nOffset, d->nFilledLen);
                }
                if (d->nFilledLen && !encode_start.tv_sec) clock_gettime(CLOCK_MONOTONIC, &encode_start);
                e->nOffset = 0; e->nFilledLen = d->nFilledLen; e->nTimeStamp = d->nTimeStamp;
                e->nFlags = OMX_BUFFERFLAG_ENDOFFRAME | (d->nFlags & OMX_BUFFERFLAG_EOS);
                encoder_input_free[i] = 0;
                if (OMX_EmptyThisBuffer(encoder, e)) return 14;
                output_free[i] = 1;
            }
            for (unsigned i = 0; i < defs[1].nBufferCountActual; i++) {
                if (atomic_exchange(&output_free[i], 0) && !decoder_eos)
                    if (OMX_FillThisBuffer(decoder, dec_out[i])) return 14;
            }
            for (int i = 0; i < enc_count; i++)
                if (atomic_exchange(&encoded_free[i], 0) && !encoder_eos)
                    if (OMX_FillThisBuffer(encoder, enc_out[i])) return 15;
        }
        for (unsigned i = 0; i < defs[0].nBufferCountActual && !sent_eos; i++) {
            if (!atomic_exchange(&input_free[i], 0)) continue;
            OMX_BUFFERHEADERTYPE *b = dec_in[i]; b->nOffset = 0; b->nFlags = OMX_BUFFERFLAG_ENDOFFRAME;
            int r, waiting = 0;
            do {
                r = pop_packet(pkt);
                if (r == 0) { waiting = 1; break; }
                r = r < 0 ? -1 : 0;
                int is_video = r >= 0 && pkt->stream_index == video_index;
                if (r >= 0 && pkt->stream_index == video_index && (pkt->flags & AV_PKT_FLAG_KEY)) seen_key = 1;
                if (r >= 0 && seen_key && pkt->stream_index == audio_index) {
                    av_packet_rescale_ts(pkt, input->streams[audio_index]->time_base, audio_out->time_base);
                    pkt->stream_index = audio_out->index;
                    pthread_mutex_lock(&mux_mutex); int a = av_interleaved_write_frame(mux, pkt); pthread_mutex_unlock(&mux_mutex);
                    if (a < 0) return 16;
                }
                if (r < 0 || (seen_key && is_video)) break;
            } while (1);
            if (waiting) { input_free[i] = 1; break; }
            if (r >= 0 && pkt->pts != AV_NOPTS_VALUE && first_pts == AV_NOPTS_VALUE) first_pts = pkt->pts;
            if (r < 0 || (first_pts != AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE &&
                          av_rescale_q(pkt->pts - first_pts, input->streams[video_index]->time_base,
                                       (AVRational){1, 1000000}) >= duration_us)) {
                b->nFilledLen = 0; b->nFlags |= OMX_BUFFERFLAG_EOS; sent_eos = 1;
                reader_stop = 1;
                pthread_mutex_lock(&input_mutex); pthread_cond_broadcast(&input_space); pthread_mutex_unlock(&input_mutex);
            } else {
                if (pkt->size < 0 || (unsigned)pkt->size > b->nAllocLen) return 17;
                memcpy(b->pBuffer, pkt->data, pkt->size); b->nFilledLen = pkt->size;
                b->nTimeStamp = pkt->pts == AV_NOPTS_VALUE ? 0 :
                    av_rescale_q(pkt->pts, input->streams[video_index]->time_base, (AVRational){1, 1000000});
            }
            ret = OMX_EmptyThisBuffer(decoder, b);
            if (ret) { fprintf(stderr, "DEC_QUEUE=%#x\n", ret); return 18; }
        }
        usleep(1000);
    }
    pthread_mutex_lock(&mux_mutex); av_write_trailer(mux); pthread_mutex_unlock(&mux_mutex);
    avio_closep(&mux->pb);
    clock_gettime(CLOCK_MONOTONIC, &encode_end);
    double elapsed = encode_start.tv_sec ? (encode_end.tv_sec - encode_start.tv_sec) +
        (encode_end.tv_nsec - encode_start.tv_nsec) / 1000000000.0 : 0.0;
    double speed = elapsed > 0 ? encoded_frames * av_q2d(av_inv_q(rate)) / elapsed : 0.0;
    fprintf(stderr, "RESULT decoded=%u encoded=%u eos=%d failed=%d processing_seconds=%.3f speed=%.3fx\n",
            decoded_frames, encoded_frames, encoder_eos, failed, elapsed, speed);
    pthread_mutex_lock(&input_mutex);
    if (reader_first_pts != AV_NOPTS_VALUE && reader_last_pts != AV_NOPTS_VALUE) {
        double media = (reader_last_pts - reader_first_pts) * av_q2d(input->streams[video_index]->time_base);
        double wall = (reader_last_time.tv_sec - reader_first_time.tv_sec) +
                      (reader_last_time.tv_nsec - reader_first_time.tv_nsec) / 1000000000.0;
        fprintf(stderr, "INPUT_TIMING media_seconds=%.3f wall_seconds=%.3f queued=%u\n", media, wall, input_count);
    }
    pthread_mutex_unlock(&input_mutex);
    if (gpu_bob_close) gpu_bob_close();
    if (gpu_bob_library) dlclose(gpu_bob_library);
    return !failed && encoder_eos && encoded_frames ? 0 : 19;
}
