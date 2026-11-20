#include "port_pool.h"

#include <pthread.h>
#include <stdlib.h>

struct port_pool {
    unsigned short *ports;
    unsigned char *used;
    int count;
    pthread_mutex_t mu;
};

static struct port_pool g_pool;
static int g_ready;

int port_pool_init(unsigned short min_port, unsigned short max_port)
{
    unsigned short p;
    int count = 0;
    int i;

    port_pool_fini();
    if ((min_port & 1u) != 0)
        min_port++;
    if (min_port == 0 || max_port < (unsigned short)(min_port + 1))
        return -1;
    for (p = min_port; p <= max_port - 1; p = (unsigned short)(p + 2)) {
        count++;
        if (p > 65533)
            break;
    }
    if (count <= 0)
        return -1;
    g_pool.ports = (unsigned short *)calloc((size_t)count, sizeof(unsigned short));
    g_pool.used = (unsigned char *)calloc((size_t)count, 1);
    if (!g_pool.ports || !g_pool.used) {
        port_pool_fini();
        return -1;
    }
    i = 0;
    for (p = min_port; i < count; p = (unsigned short)(p + 2), i++)
        g_pool.ports[i] = p;
    g_pool.count = count;
    pthread_mutex_init(&g_pool.mu, NULL);
    g_ready = 1;
    return 0;
}

void port_pool_fini(void)
{
    if (g_ready)
        pthread_mutex_destroy(&g_pool.mu);
    free(g_pool.ports);
    free(g_pool.used);
    g_pool.ports = NULL;
    g_pool.used = NULL;
    g_pool.count = 0;
    g_ready = 0;
}

int port_pool_alloc(unsigned short *rtp_port)
{
    int i;

    if (!rtp_port || !g_ready)
        return -1;
    pthread_mutex_lock(&g_pool.mu);
    for (i = 0; i < g_pool.count; i++) {
        if (!g_pool.used[i]) {
            g_pool.used[i] = 1;
            *rtp_port = g_pool.ports[i];
            pthread_mutex_unlock(&g_pool.mu);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_pool.mu);
    return -1;
}

void port_pool_free(unsigned short rtp_port)
{
    int i;

    if (!g_ready || rtp_port == 0)
        return;
    pthread_mutex_lock(&g_pool.mu);
    for (i = 0; i < g_pool.count; i++) {
        if (g_pool.ports[i] == rtp_port) {
            g_pool.used[i] = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_pool.mu);
}
