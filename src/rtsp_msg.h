#ifndef RTSPS_RTSP_MSG_H
#define RTSPS_RTSP_MSG_H

#include <stddef.h>

#define RTSP_URL 512
#define RTSP_TRANSPORT 256
#define RTSP_SESSION 64

struct rtsp_request {
    char method[32];
    char url[RTSP_URL];
    char session[RTSP_SESSION];
    char transport[RTSP_TRANSPORT];
    int cseq;
    int has_cseq;
};

enum {
    RTSP_TP_TCP = 1,
    RTSP_TP_UDP = 2,
    RTSP_TP_MCAST = 3
};

/* 1 已取出一条完整请求，0 还不够，-1 无法解析。 */
int rtsp_parse_request(const unsigned char *buf, size_t len,
                       struct rtsp_request *req, size_t *consumed);

int rtsp_reply(int fd, int code, int cseq, const char *headers, const char *body);

void rtsp_stream_name(const char *url, char *name, size_t cap);
int rtsp_content_base(const char *url, const char *name, char *out, size_t cap);
int rtsp_content_base_from_fd(int fd, const char *name, char *out, size_t cap);

/* 1 TCP 交织，2 UDP，3 组播，-1 拒绝。 */
int rtsp_transport_kind(const char *transport);
int rtsp_parse_port_pair(const char *transport, const char *key, int *a, int *b);
int rtsp_format_tcp_transport(char *out, size_t cap, int ch0, int ch1);
int rtsp_format_udp_transport(char *out, size_t cap, int c0, int c1, int s0, int s1);
int rtsp_format_mcast_transport(char *out, size_t cap, const char *addr, int port, int ttl);

#endif
