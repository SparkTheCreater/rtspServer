#ifndef RTSPS_SESSION_H
#define RTSPS_SESSION_H

#include "rtsps_internal.h"

struct rtsp_session *session_create(struct rtsps_server *sv, int fd);
int session_start(struct rtsp_session *s);
void session_request_stop(struct rtsp_session *s);
void session_join(struct rtsp_session *s);
void session_free(struct rtsp_session *s);
int session_exited(struct rtsp_session *s);
void session_drop_stream(struct rtsp_session *s, struct media_stream *st);

#endif
