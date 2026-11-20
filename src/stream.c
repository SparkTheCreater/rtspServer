#include "stream.h"

#include "annexb.h"
#include "filter.h"
#include "log.h"
#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t abs_ns, volatile int *running)
{
    while (*running) {
        uint64_t now = now_ns();
        uint64_t left;
        struct timespec ts;
        if (now >= abs_ns)
            return;
        left = abs_ns - now;
        if (left > 50000000ull)
            left = 50000000ull;
        ts.tv_sec = (time_t)(left / 1000000000ull);
        ts.tv_nsec = (long)(left % 1000000000ull);
        nanosleep(&ts, NULL);
    }
}

static int name_ok(const char *name)
{
    size_t n;
    if (!name || !name[0])
        return 0;
    n = strlen(name);
    if (n >= RTS_NAME)
        return 0;
    if (strchr(name, '/') || strchr(name, ' '))
        return 0;
    return 1;
}

static struct media_stream *find_unlocked(struct rtsps_server *sv, const char *name)
{
    int i;
    for (i = 0; i < RTS_MAX_STREAMS; i++) {
        if (sv->streams[i].used && !sv->streams[i].removed &&
            strcmp(sv->streams[i].name, name) == 0)
            return &sv->streams[i];
    }
    return NULL;
}

static struct media_stream *free_slot(struct rtsps_server *sv)
{
    int i;
    for (i = 0; i < RTS_MAX_STREAMS; i++) {
        if (!sv->streams[i].used)
            return &sv->streams[i];
    }
    return NULL;
}

static void fanout(void *user, const struct frame *frame)
{
    struct media_stream *st = (struct media_stream *)user;
    int i;

    pthread_mutex_lock(&st->mu);
    if (st->removed) {
        pthread_mutex_unlock(&st->mu);
        return;
    }
    for (i = 0; i < RTS_MAX_SUBS; i++) {
        if (st->subs[i].used && st->subs[i].on_frame)
            st->subs[i].on_frame(st->subs[i].user, frame);
    }
    pthread_mutex_unlock(&st->mu);
}

static void stream_emit(struct media_stream *st, const uint8_t *data, size_t len,
                        uint32_t pts, int marker)
{
    struct frame fr;
    fr.data = (uint8_t *)data;
    fr.len = len;
    fr.pts90k = pts;
    fr.marker = marker;
    if (!st->filter.push)
        return;
    st->filter.push(&st->filter, &fr, fanout, st);
}

static void clear_subs(struct media_stream *st)
{
    int i;
    pthread_mutex_lock(&st->mu);
    for (i = 0; i < RTS_MAX_SUBS; i++) {
        st->subs[i].used = 0;
        st->subs[i].on_frame = NULL;
        st->subs[i].user = NULL;
    }
    pthread_mutex_unlock(&st->mu);
}

static int hold_push(uint8_t **held, size_t *held_len, int *nheld,
                     const uint8_t *nal, size_t len)
{
    uint8_t *copy;
    if (*nheld == 8) {
        free(held[0]);
        memmove(held, held + 1, 7 * sizeof(held[0]));
        memmove(held_len, held_len + 1, 7 * sizeof(held_len[0]));
        *nheld = 7;
    }
    copy = (uint8_t *)malloc(len);
    if (!copy)
        return -1;
    memcpy(copy, nal, len);
    held[*nheld] = copy;
    held_len[*nheld] = len;
    (*nheld)++;
    return 0;
}

static void hold_free(uint8_t **held, int nheld)
{
    int i;
    for (i = 0; i < nheld; i++)
        free(held[i]);
}

static int emit_access_unit(struct media_stream *st, const uint8_t *nal, size_t len,
                            uint8_t **held, size_t *held_len, int *nheld,
                            uint32_t *pts, uint64_t *next_ns, uint64_t interval, uint32_t step)
{
    int i;
    if (len == 0)
        return 0;
    if (!annexb_is_vcl(nal, len))
        return hold_push(held, held_len, nheld, nal, len);
    if (*next_ns < now_ns())
        *next_ns = now_ns();
    sleep_until(*next_ns, &st->running);
    if (!st->running)
        return -1;
    *next_ns += interval;
    for (i = 0; i < *nheld; i++) {
        stream_emit(st, held[i], held_len[i], *pts, 0);
        free(held[i]);
        held[i] = NULL;
    }
    *nheld = 0;
    stream_emit(st, nal, len, *pts, 1);
    *pts += step;
    return 0;
}

/* 1 读到新字节，0 文件正常结束，-1 读失败或缓冲无法继续。 */
static int file_read_more(FILE *fp, uint8_t **buf, size_t *len, size_t *cap)
{
    size_t nread;

    if (*len == *cap) {
        if (*cap < RTS_MAX_NAL) {
            size_t ncap = *cap * 2;
            uint8_t *nbuf;
            if (ncap > RTS_MAX_NAL)
                ncap = RTS_MAX_NAL;
            if (ncap > *cap) {
                nbuf = (uint8_t *)realloc(*buf, ncap);
                if (!nbuf)
                    return -1;
                *buf = nbuf;
                *cap = ncap;
            }
        }
        if (*len == *cap) {
            if (*len <= 4)
                return -1;
            memmove(*buf, *buf + *len - 4, 4);
            *len = 4;
        }
    }
    nread = fread(*buf + *len, 1, *cap - *len, fp);
    if (nread > 0) {
        *len += nread;
        return 1;
    }
    if (ferror(fp))
        return -1;
    return 0;
}

static int file_rewind(FILE *fp)
{
    if (fseek(fp, 0, SEEK_SET) != 0)
        return -1;
    clearerr(fp);
    return 0;
}

static void *file_thread(void *arg)
{
    struct media_stream *st = (struct media_stream *)arg;
    FILE *fp;
    size_t cap = 256 * 1024;
    uint8_t *buf;
    size_t len = 0;
    uint32_t pts = 0;
    uint32_t step;
    uint64_t interval;
    uint64_t next_ns;
    uint8_t *held[8];
    size_t held_len[8];
    int nheld = 0;
    int fps;

    fp = fopen(st->path, "rb");
    if (!fp) {
        rtsps_log("open file failed: %s", st->path);
        return NULL;
    }
    buf = (uint8_t *)malloc(cap);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    fps = st->fps > 0 ? st->fps : 25;
    step = 90000u / (uint32_t)fps;
    if (step == 0)
        step = 1;
    interval = 1000000000ull / (uint64_t)fps;
    next_ns = now_ns();
    rtsps_log("file stream %s fps %d", st->name, fps);

    while (st->running) {
        size_t off = 0;
        int emitted = 0;
        int rd;
        while (st->running) {
            const uint8_t *nal = NULL;
            size_t nlen = 0;
            size_t next = off;
            if (!annexb_next(buf, len, &next, &nal, &nlen))
                break;
            if (emit_access_unit(st, nal, nlen, held, held_len, &nheld,
                                 &pts, &next_ns, interval, step) != 0)
                goto out;
            off = next;
            emitted = 1;
        }
        if (off > 0) {
            memmove(buf, buf + off, len - off);
            len -= off;
        }
        if (!st->running)
            break;

        rd = file_read_more(fp, &buf, &len, &cap);
        if (rd > 0)
            continue;
        if (rd < 0)
            break;

        /* 确认是文件结束，不是缓冲刚好读满。把最后一个 NALU 送出后回到开头。 */
        {
            const uint8_t *nal = NULL;
            size_t nlen = 0;
            if (annexb_tail(buf, len, &nal, &nlen)) {
                if (emit_access_unit(st, nal, nlen, held, held_len, &nheld,
                                     &pts, &next_ns, interval, step) != 0)
                    goto out;
                emitted = 1;
            }
        }
        len = 0;
        if (file_rewind(fp) != 0)
            break;
        rtsps_log("file rewind %s", st->name);
        if (!emitted) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 100000000L;
            nanosleep(&ts, NULL);
        }
    }
out:
    hold_free(held, nheld);
    free(buf);
    fclose(fp);
    return NULL;
}

static int start_file_thread(struct media_stream *st)
{
    if (!st->is_file || st->thread_on)
        return 0;
    st->running = 1;
    if (pthread_create(&st->thread, NULL, file_thread, st) != 0) {
        st->running = 0;
        return -1;
    }
    st->thread_on = 1;
    return 0;
}

static void stop_file_thread(struct media_stream *st)
{
    if (!st->thread_on)
        return;
    st->running = 0;
    pthread_join(st->thread, NULL);
    st->thread_on = 0;
}

static int add_common(struct rtsps_server *sv, const char *name, int fps, int is_file, const char *path)
{
    struct media_stream *st;
    if (!name_ok(name))
        return -1;
    if (fps <= 0)
        fps = 25;
    pthread_mutex_lock(&sv->mu);
    if (find_unlocked(sv, name)) {
        pthread_mutex_unlock(&sv->mu);
        return -1;
    }
    st = free_slot(sv);
    if (!st) {
        pthread_mutex_unlock(&sv->mu);
        return -1;
    }
    st->used = 1;
    st->removed = 0;
    st->is_file = is_file;
    st->fps = fps;
    st->running = 0;
    st->thread_on = 0;
    st->gen++;
    if (st->gen == 0)
        st->gen = 1;
    snprintf(st->name, sizeof(st->name), "%s", name);
    st->path[0] = '\0';
    if (path)
        snprintf(st->path, sizeof(st->path), "%s", path);
    memset(st->subs, 0, sizeof(st->subs));
    filter_copy_init(&st->filter);
    pthread_mutex_unlock(&sv->mu);
    if (sv->running && is_file && start_file_thread(st) != 0) {
        pthread_mutex_lock(&sv->mu);
        st->used = 0;
        st->removed = 1;
        st->name[0] = '\0';
        pthread_mutex_unlock(&sv->mu);
        return -1;
    }
    return 0;
}

int stream_add_file(struct rtsps_server *sv, const char *name, const char *path, int fps)
{
    FILE *fp;
    if (!sv || !path || strlen(path) >= RTS_PATH)
        return -1;
    fp = fopen(path, "rb");
    if (!fp)
        return -1;
    fclose(fp);
    return add_common(sv, name, fps, 1, path);
}

int stream_add_live(struct rtsps_server *sv, const char *name, int fps)
{
    if (!sv)
        return -1;
    return add_common(sv, name, fps, 0, NULL);
}

int stream_remove(struct rtsps_server *sv, const char *name)
{
    struct media_stream *st;
    int i;
    if (!sv || !name)
        return -1;
    pthread_mutex_lock(&sv->mu);
    st = find_unlocked(sv, name);
    if (!st) {
        pthread_mutex_unlock(&sv->mu);
        return -1;
    }
    st->removed = 1;
    st->used = 0;
    st->name[0] = '\0';
    pthread_mutex_lock(&st->mu);
    st->removed = 1;
    pthread_mutex_unlock(&st->mu);
    for (i = 0; i < RTS_MAX_SESSIONS; i++) {
        if (sv->sessions[i])
            session_drop_stream(sv->sessions[i], st);
    }
    pthread_mutex_unlock(&sv->mu);
    stop_file_thread(st);
    clear_subs(st);
    if (st->filter.close)
        st->filter.close(&st->filter);
    return 0;
}

int stream_input(struct rtsps_server *sv, const char *name,
                 const unsigned char *nal, int len, unsigned int pts90k)
{
    struct media_stream *st;
    int marker;
    if (!sv || !name || !nal || len <= 0 || (size_t)len > RTS_MAX_NAL)
        return -1;
    pthread_mutex_lock(&sv->mu);
    st = find_unlocked(sv, name);
    pthread_mutex_unlock(&sv->mu);
    if (!st)
        return -1;
    marker = annexb_is_vcl(nal, (size_t)len);
    stream_emit(st, nal, (size_t)len, pts90k, marker);
    return len;
}

void stream_start_all(struct rtsps_server *sv)
{
    int i;
    for (i = 0; i < RTS_MAX_STREAMS; i++) {
        if (sv->streams[i].used && sv->streams[i].is_file)
            start_file_thread(&sv->streams[i]);
    }
}

void stream_stop_all(struct rtsps_server *sv)
{
    int i;
    for (i = 0; i < RTS_MAX_STREAMS; i++) {
        if (sv->streams[i].thread_on)
            stop_file_thread(&sv->streams[i]);
    }
}

struct media_stream *stream_find(struct rtsps_server *sv, const char *name)
{
    struct media_stream *st;
    if (!sv || !name || !name[0])
        return NULL;
    pthread_mutex_lock(&sv->mu);
    st = find_unlocked(sv, name);
    pthread_mutex_unlock(&sv->mu);
    return st;
}

uint32_t stream_gen(struct media_stream *st)
{
    uint32_t gen;
    if (!st)
        return 0;
    pthread_mutex_lock(&st->mu);
    gen = st->gen;
    pthread_mutex_unlock(&st->mu);
    return gen;
}

int stream_subscribe(struct media_stream *st, uint32_t gen,
                     void (*on_frame)(void *user, const struct frame *frame),
                     void *user)
{
    int i;
    if (!st || !on_frame)
        return -1;
    pthread_mutex_lock(&st->mu);
    if (!st->used || st->removed || st->gen != gen) {
        pthread_mutex_unlock(&st->mu);
        return -1;
    }
    for (i = 0; i < RTS_MAX_SUBS; i++) {
        if (st->subs[i].used && st->subs[i].user == user) {
            pthread_mutex_unlock(&st->mu);
            return 0;
        }
    }
    for (i = 0; i < RTS_MAX_SUBS; i++) {
        if (!st->subs[i].used) {
            st->subs[i].used = 1;
            st->subs[i].on_frame = on_frame;
            st->subs[i].user = user;
            pthread_mutex_unlock(&st->mu);
            return 0;
        }
    }
    pthread_mutex_unlock(&st->mu);
    return -1;
}

void stream_unsubscribe(struct media_stream *st, void *user)
{
    int i;
    if (!st)
        return;
    pthread_mutex_lock(&st->mu);
    for (i = 0; i < RTS_MAX_SUBS; i++) {
        if (st->subs[i].used && st->subs[i].user == user) {
            st->subs[i].used = 0;
            st->subs[i].on_frame = NULL;
            st->subs[i].user = NULL;
        }
    }
    pthread_mutex_unlock(&st->mu);
}
