#include "rtsp_server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

int main(int argc, char **argv)
{
    unsigned long port_ul;
    int fps;
    int handle = -1;
    int started = 0;

    if (argc < 5) {
        fprintf(stderr, "usage: demo <port> <name> <file.h264> <fps>\n");
        return 1;
    }
    port_ul = strtoul(argv[1], NULL, 10);
    fps = atoi(argv[4]);
    if (port_ul == 0 || port_ul > 65535) {
        fprintf(stderr, "bad port\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (RTSPServer_InitLib(1, 30000, 30500) != 0) {
        fprintf(stderr, "RTSPServer_InitLib failed\n");
        return 1;
    }
    handle = RTSPServer_Create((unsigned short)port_ul);
    if (handle < 0) {
        fprintf(stderr, "RTSPServer_Create failed\n");
        goto fail;
    }
    if (RTSPServer_AddFileStream(handle, argv[2], argv[3], fps) != 0) {
        fprintf(stderr, "RTSPServer_AddFileStream failed\n");
        goto fail;
    }
    if (RTSPServer_Start(handle) != 0) {
        fprintf(stderr, "RTSPServer_Start failed\n");
        goto fail;
    }
    started = 1;
    printf("rtsp://127.0.0.1:%lu/%s\n", port_ul, argv[2]);
    fflush(stdout);

    while (!g_stop)
        sleep(1);

    RTSPServer_Stop(handle);
    RTSPServer_Destroy(handle);
    RTSPServer_FiniLib();
    return 0;

fail:
    if (handle >= 0) {
        if (started)
            RTSPServer_Stop(handle);
        RTSPServer_Destroy(handle);
    }
    RTSPServer_FiniLib();
    return 1;
}
