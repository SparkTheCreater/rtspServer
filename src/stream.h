#ifndef RTSPS_STREAM_H
#define RTSPS_STREAM_H

#include "rtsps_internal.h"

#include <stdint.h>

int stream_add_file(struct rtsps_server *sv, const char *name, const char *path, int fps);
int stream_add_live(struct rtsps_server *sv, const char *name, int fps);
int stream_remove(struct rtsps_server *sv, const char *name);
int stream_input(struct rtsps_server *sv, const char *name,
                 const unsigned char *nal, int len, unsigned int pts90k);
void stream_start_all(struct rtsps_server *sv);
void stream_stop_all(struct rtsps_server *sv);
struct media_stream *stream_find(struct rtsps_server *sv, const char *name);

uint32_t stream_gen(struct media_stream *st);
int stream_subscribe(struct media_stream *st, uint32_t gen,
                     void (*on_frame)(void *user, const struct frame *frame),
                     void *user);
void stream_unsubscribe(struct media_stream *st, void *user);

#endif
