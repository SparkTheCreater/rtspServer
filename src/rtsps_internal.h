#ifndef RTSPS_INTERNAL_H
#define RTSPS_INTERNAL_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define RTS_MAX_SERVERS 4
#define RTS_MAX_STREAMS 8
#define RTS_MAX_SESSIONS 8
#define RTS_MAX_SUBS 8
#define RTS_QDEPTH 16
#define RTS_MAX_NAL (512 * 1024)
#define RTS_NAME 64
#define RTS_PATH 512

struct frame {
    uint8_t *data;
    size_t len;
    uint32_t pts90k;
    int marker;
};

struct frame_filter {
    const char *name;
    int (*open)(struct frame_filter *self);
    int (*push)(struct frame_filter *self, const struct frame *in,
                void (*emit)(void *user, const struct frame *out), void *user);
    void (*close)(struct frame_filter *self);
    void *priv;
};

struct subscriber {
    int used;
    void (*on_frame)(void *user, const struct frame *frame);
    void *user;
};

struct media_stream {
    int used;
    int removed;
    uint32_t epoch;
    int is_file;
    char name[RTS_NAME];
    char path[RTS_PATH];
    int fps;
    uint32_t gen;
    volatile int running;
    int thread_on;
    pthread_t thread;
    pthread_mutex_t mu;
    struct frame_filter filter;
    struct subscriber subs[RTS_MAX_SUBS];
};

struct rtsp_session;

struct rtsps_server {
    int used;
    unsigned short port;
    int listen_fd;
    int wake_r;
    int wake_w;
    volatile int running;
    int thread_on;
    pthread_t accept_thread;
    pthread_mutex_t mu;
    struct media_stream streams[RTS_MAX_STREAMS];
    struct rtsp_session *sessions[RTS_MAX_SESSIONS];
};

#endif
