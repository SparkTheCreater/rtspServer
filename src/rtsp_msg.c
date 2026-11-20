#include "rtsp_msg.h"

#include "io.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static const char *reason_of(int code)
{
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 454: return "Session Not Found";
    case 455: return "Method Not Valid in This State";
    case 461: return "Unsupported transport";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default: return "OK";
    }
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static int header_value(const char *headers, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = headers;

    if (!out || cap == 0)
        return -1;
    out[0] = '\0';
    while (*p) {
        const char *line = p;
        const char *nl = strstr(p, "\r\n");
        size_t line_len;
        if (!nl)
            break;
        line_len = (size_t)(nl - line);
        if (line_len > klen && strncasecmp(line, key, klen) == 0 && line[klen] == ':') {
            const char *v = skip_ws(line + klen + 1);
            size_t n = (size_t)(nl - v);
            if (n >= cap)
                n = cap - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return 0;
        }
        p = nl + 2;
    }
    return -1;
}

int rtsp_parse_request(const unsigned char *buf, size_t len,
                       struct rtsp_request *req, size_t *consumed)
{
    const unsigned char *end = NULL;
    size_t i;
    size_t header_len;
    size_t msg_len;
    char headers[8192];
    char cl[32];
    int content_len = 0;
    char *line_end;
    char *cursor;

    if (!buf || !req || !consumed)
        return -1;
    for (i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            end = buf + i;
            break;
        }
    }
    if (!end)
        return 0;
    header_len = (size_t)(end - buf);
    if (header_len >= sizeof(headers))
        return -1;
    memcpy(headers, buf, header_len);
    headers[header_len] = '\0';
    if (header_value(headers, "Content-Length", cl, sizeof(cl)) == 0) {
        content_len = atoi(cl);
        if (content_len < 0 || content_len > 65536)
            return -1;
    }
    msg_len = header_len + 4 + (size_t)content_len;
    if (len < msg_len)
        return 0;

    memset(req, 0, sizeof(*req));
    line_end = strstr(headers, "\r\n");
    if (!line_end)
        return -1;
    *line_end = '\0';
    if (sscanf(headers, "%31s %511s %15s", req->method, req->url, cl) != 3)
        return -1;
    if (strncasecmp(cl, "RTSP/", 5) != 0)
        return -1;
    cursor = line_end + 2;
    if (header_value(cursor, "CSeq", cl, sizeof(cl)) == 0) {
        req->cseq = atoi(cl);
        req->has_cseq = 1;
    }
    header_value(cursor, "Session", req->session, sizeof(req->session));
    header_value(cursor, "Transport", req->transport, sizeof(req->transport));
    *consumed = msg_len;
    return 1;
}

int rtsp_reply(int fd, int code, int cseq, const char *headers, const char *body)
{
    char head[1536];
    size_t blen = body ? strlen(body) : 0;
    int n;

    if (body) {
        n = snprintf(head, sizeof(head),
                     "RTSP/1.0 %d %s\r\n"
                     "CSeq: %d\r\n"
                     "%s"
                     "Content-Length: %zu\r\n"
                     "\r\n",
                     code, reason_of(code), cseq,
                     headers ? headers : "",
                     blen);
    } else {
        n = snprintf(head, sizeof(head),
                     "RTSP/1.0 %d %s\r\n"
                     "CSeq: %d\r\n"
                     "%s"
                     "\r\n",
                     code, reason_of(code), cseq,
                     headers ? headers : "");
    }
    if (n < 0 || (size_t)n >= sizeof(head))
        return -1;
    if (io_write_all(fd, head, (size_t)n, NULL, NULL) != 0)
        return -1;
    if (blen > 0 && io_write_all(fd, body, blen, NULL, NULL) != 0)
        return -1;
    return 0;
}

void rtsp_stream_name(const char *url, char *name, size_t cap)
{
    const char *path;
    const char *scheme;
    size_t i = 0;

    if (!name || cap == 0)
        return;
    name[0] = '\0';
    if (!url || strcmp(url, "*") == 0)
        return;
    scheme = strstr(url, "://");
    path = scheme ? strchr(scheme + 3, '/') : url;
    if (!path)
        return;
    if (*path == '/')
        path++;
    while (path[i] && path[i] != '/' && path[i] != '?' && path[i] != ' ' && i + 1 < cap) {
        name[i] = path[i];
        i++;
    }
    name[i] = '\0';
}

int rtsp_content_base(const char *url, const char *name, char *out, size_t cap)
{
    const char *p;
    const char *slash;
    size_t auth_len;
    int n;

    if (!url || !name || !name[0] || !out)
        return -1;
    p = strstr(url, "://");
    if (!p)
        return -1;
    p += 3;
    slash = strchr(p, '/');
    auth_len = slash ? (size_t)(slash - p) : strlen(p);
    if (auth_len == 0 || auth_len > 256)
        return -1;
    n = snprintf(out, cap, "rtsp://%.*s/%s/", (int)auth_len, p, name);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return 0;
}

int rtsp_content_base_from_fd(int fd, const char *name, char *out, size_t cap)
{
    struct sockaddr_in addr;
    socklen_t sl = sizeof(addr);
    char ip[64];
    int n;

    if (!name || !out || getsockname(fd, (struct sockaddr *)&addr, &sl) != 0)
        return -1;
    if (addr.sin_family != AF_INET)
        return -1;
    if (!inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip)))
        return -1;
    n = snprintf(out, cap, "rtsp://%s:%u/%s/", ip, ntohs(addr.sin_port), name);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return 0;
}

int rtsp_transport_kind(const char *transport)
{
    if (!transport || !transport[0])
        return -1;
    if (strcasestr(transport, "interleaved="))
        return RTSP_TP_TCP;
    if (strcasestr(transport, "multicast"))
        return -1;
    if (strcasestr(transport, "RTP/AVP/TCP"))
        return -1;
    if (strcasestr(transport, "client_port="))
        return RTSP_TP_UDP;
    return -1;
}

int rtsp_parse_port_pair(const char *transport, const char *key, int *a, int *b)
{
    size_t klen;
    const char *p;

    if (!transport || !key || !a || !b)
        return -1;
    klen = strlen(key);
    p = transport;
    while ((p = strcasestr(p, key)) != NULL) {
        int boundary = (p == transport) || p[-1] == ';' || p[-1] == ' ' || p[-1] == '\t';
        if (boundary && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *dash = strchr(v, '-');
            const char *semi = strchr(v, ';');
            if (!dash || (semi && dash > semi))
                return -1;
            *a = atoi(v);
            *b = atoi(dash + 1);
            return 0;
        }
        p += klen;
    }
    return -1;
}

int rtsp_format_tcp_transport(char *out, size_t cap, int ch0, int ch1)
{
    int n = snprintf(out, cap, "RTP/AVP/TCP;unicast;interleaved=%d-%d", ch0, ch1);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return 0;
}

int rtsp_format_udp_transport(char *out, size_t cap, int c0, int c1, int s0, int s1)
{
    int n = snprintf(out, cap,
                     "RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d",
                     c0, c1, s0, s1);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return 0;
}
