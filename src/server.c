#include "rtsp_server.h"

#include "io.h"
#include "log.h"
#include "port_pool.h"
#include "rtsps_internal.h"
#include "session.h"
#include "stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static struct rtsps_server *g_servers[RTS_MAX_SERVERS];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_inited;

static struct rtsps_server *server_get(int handle)
{
    if (handle < 0 || handle >= RTS_MAX_SERVERS)
        return NULL;
    if (!g_servers[handle] || !g_servers[handle]->used)
        return NULL;
    return g_servers[handle];
}

static void reap_sessions(struct rtsps_server *sv)
{
    int i;
    pthread_mutex_lock(&sv->mu);
    for (i = 0; i < RTS_MAX_SESSIONS; i++) {
        struct rtsp_session *s = sv->sessions[i];
        if (!s || !session_exited(s))
            continue;
        sv->sessions[i] = NULL;
        pthread_mutex_unlock(&sv->mu);
        session_join(s);
        session_free(s);
        pthread_mutex_lock(&sv->mu);
    }
    pthread_mutex_unlock(&sv->mu);
}

static int attach_client(struct rtsps_server *sv, int fd)
{
    struct rtsp_session *s;
    int i;

    pthread_mutex_lock(&sv->mu);
    if (!sv->running) {
        pthread_mutex_unlock(&sv->mu);
        close(fd);
        return -1;
    }
    for (i = 0; i < RTS_MAX_SESSIONS; i++) {
        if (!sv->sessions[i])
            break;
    }
    if (i == RTS_MAX_SESSIONS) {
        pthread_mutex_unlock(&sv->mu);
        close(fd);
        rtsps_log("reject client, session table full");
        return -1;
    }
    s = session_create(sv, fd);
    if (!s || session_start(s) != 0) {
        pthread_mutex_unlock(&sv->mu);
        if (s)
            session_free(s);
        else
            close(fd);
        return -1;
    }
    sv->sessions[i] = s;
    pthread_mutex_unlock(&sv->mu);
    return 0;
}

static void stop_sessions(struct rtsps_server *sv)
{
    struct rtsp_session *list[RTS_MAX_SESSIONS];
    int n = 0;
    int i;

    pthread_mutex_lock(&sv->mu);
    for (i = 0; i < RTS_MAX_SESSIONS; i++) {
        if (sv->sessions[i]) {
            list[n++] = sv->sessions[i];
            sv->sessions[i] = NULL;
        }
    }
    pthread_mutex_unlock(&sv->mu);
    for (i = 0; i < n; i++) {
        session_request_stop(list[i]);
        session_join(list[i]);
        session_free(list[i]);
    }
}

static void *accept_main(void *arg)
{
    struct rtsps_server *sv = (struct rtsps_server *)arg;

    while (sv->running) {
        struct pollfd pf[2];
        int pr;
        pf[0].fd = sv->listen_fd;
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        pf[1].fd = sv->wake_r;
        pf[1].events = POLLIN;
        pf[1].revents = 0;
        pr = poll(pf, 2, 200);
        if (!sv->running)
            break;
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pf[1].revents)
            break;
        reap_sessions(sv);
        if (pr == 0 || (pf[0].revents & POLLIN) == 0)
            continue;
        {
            int fd = accept(sv->listen_fd, NULL, NULL);
            if (fd < 0)
                continue;
            attach_client(sv, fd);
        }
    }
    return NULL;
}

static void close_listen(struct rtsps_server *sv)
{
    if (sv->listen_fd >= 0) {
        close(sv->listen_fd);
        sv->listen_fd = -1;
    }
    if (sv->wake_r >= 0) {
        close(sv->wake_r);
        sv->wake_r = -1;
    }
    if (sv->wake_w >= 0) {
        close(sv->wake_w);
        sv->wake_w = -1;
    }
}

int RTSPServer_InitLib(int dbgLevel, unsigned short minPort, unsigned short maxPort)
{
    int rc;
    pthread_mutex_lock(&g_mu);
    if (g_inited) {
        pthread_mutex_unlock(&g_mu);
        return 0;
    }
    rtsps_set_debug(dbgLevel);
    rc = port_pool_init(minPort, maxPort);
    if (rc == 0)
        g_inited = 1;
    pthread_mutex_unlock(&g_mu);
    return rc;
}

int RTSPServer_FiniLib(void)
{
    int i;
    pthread_mutex_lock(&g_mu);
    if (!g_inited) {
        pthread_mutex_unlock(&g_mu);
        return 0;
    }
    for (i = 0; i < RTS_MAX_SERVERS; i++) {
        if (g_servers[i]) {
            pthread_mutex_unlock(&g_mu);
            RTSPServer_Destroy(i);
            pthread_mutex_lock(&g_mu);
        }
    }
    port_pool_fini();
    g_inited = 0;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

int RTSPServer_Create(unsigned short listenPort)
{
    struct rtsps_server *sv;
    int handle = -1;
    int i;

    pthread_mutex_lock(&g_mu);
    if (!g_inited) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    for (i = 0; i < RTS_MAX_SERVERS; i++) {
        if (!g_servers[i]) {
            handle = i;
            break;
        }
    }
    if (handle < 0) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    sv = (struct rtsps_server *)calloc(1, sizeof(*sv));
    if (!sv) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    sv->used = 1;
    sv->port = listenPort;
    sv->listen_fd = -1;
    sv->wake_r = -1;
    sv->wake_w = -1;
    pthread_mutex_init(&sv->mu, NULL);
    for (i = 0; i < RTS_MAX_STREAMS; i++)
        pthread_mutex_init(&sv->streams[i].mu, NULL);
    g_servers[handle] = sv;
    pthread_mutex_unlock(&g_mu);
    return handle;
}

static int server_stop(struct rtsps_server *sv)
{
    unsigned char b = 1;
    if (!sv)
        return -1;
    sv->running = 0;
    if (sv->wake_w >= 0)
        (void)write(sv->wake_w, &b, 1);
    if (sv->thread_on) {
        pthread_join(sv->accept_thread, NULL);
        sv->thread_on = 0;
    }
    stop_sessions(sv);
    stream_stop_all(sv);
    close_listen(sv);
    return 0;
}

int RTSPServer_Destroy(int hServer)
{
    struct rtsps_server *sv;
    int i;

    pthread_mutex_lock(&g_mu);
    sv = server_get(hServer);
    if (!sv) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }
    g_servers[hServer] = NULL;
    pthread_mutex_unlock(&g_mu);

    server_stop(sv);
    for (i = 0; i < RTS_MAX_STREAMS; i++) {
        if (sv->streams[i].filter.close && sv->streams[i].used)
            sv->streams[i].filter.close(&sv->streams[i].filter);
        pthread_mutex_destroy(&sv->streams[i].mu);
    }
    pthread_mutex_destroy(&sv->mu);
    free(sv);
    return 0;
}

int RTSPServer_AddFileStream(int hServer, const char *name, const char *path, int fps)
{
    struct rtsps_server *sv;
    int rc;
    pthread_mutex_lock(&g_mu);
    sv = server_get(hServer);
    pthread_mutex_unlock(&g_mu);
    if (!sv)
        return -1;
    rc = stream_add_file(sv, name, path, fps);
    if (rc == 0)
        rtsps_log("add file %s -> %s", name, path);
    return rc;
}

int RTSPServer_AddStream(int hServer, const char *name, int fps)
{
    struct rtsps_server *sv;
    pthread_mutex_lock(&g_mu);
    sv = server_get(hServer);
    pthread_mutex_unlock(&g_mu);
    if (!sv)
        return -1;
    return stream_add_live(sv, name, fps);
}

int RTSPServer_InputFrame(int hServer, const char *name,
                          const unsigned char *nal, int len, unsigned int pts90k)
{
    struct rtsps_server *sv;
    pthread_mutex_lock(&g_mu);
    sv = server_get(hServer);
    pthread_mutex_unlock(&g_mu);
    if (!sv)
        return -1;
    return stream_input(sv, name, nal, len, pts90k);
}

int RTSPServer_RemoveStream(int hServer, const char *name)
{
    struct rtsps_server *sv;
    pthread_mutex_lock(&g_mu);
    sv = server_get(hServer);
    pthread_mutex_unlock(&g_mu);
    if (!sv)
        return -1;
    return stream_remove(sv, name);
}

int RTSPServer_Start(int hServer)
{
    struct rtsps_server *sv;
    int fd = -1;
    int pipes[2] = {-1, -1};
    int on = 1;
    struct sockaddr_in addr;

    pthread_mutex_lock(&g_mu);
    sv = server_get(hServer);
    pthread_mutex_unlock(&g_mu);
    if (!sv)
        return -1;
    if (sv->running)
        return 0;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(sv->port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) {
        close(fd);
        rtsps_log("listen %u failed", sv->port);
        return -1;
    }
    io_set_nonblock(fd);
    if (pipe(pipes) != 0) {
        close(fd);
        return -1;
    }
    io_set_nonblock(pipes[0]);
    io_set_nonblock(pipes[1]);
    sv->listen_fd = fd;
    sv->wake_r = pipes[0];
    sv->wake_w = pipes[1];
    sv->running = 1;
    stream_start_all(sv);
    if (pthread_create(&sv->accept_thread, NULL, accept_main, sv) != 0) {
        sv->running = 0;
        stream_stop_all(sv);
        close_listen(sv);
        return -1;
    }
    sv->thread_on = 1;
    rtsps_log("listen %u", sv->port);
    return 0;
}

int RTSPServer_Stop(int hServer)
{
    struct rtsps_server *sv;
    pthread_mutex_lock(&g_mu);
    sv = server_get(hServer);
    pthread_mutex_unlock(&g_mu);
    if (!sv)
        return -1;
    return server_stop(sv);
}
