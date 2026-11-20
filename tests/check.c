#include "annexb.h"
#include "rtp_h264.h"
#include "rtsp_msg.h"
#include "sdp.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "fail: %s\n", msg);
        g_fail++;
    }
}

static void test_annexb(void)
{
    const unsigned char buf[] = {
        0, 0, 0, 1, 0x67, 0xaa,
        0, 0, 1, 0x68, 0xbb,
        0, 0, 0, 1, 0x65, 0xcc, 0xdd
    };
    const unsigned char *nal = NULL;
    size_t nlen = 0;
    size_t off = 0;

    expect(annexb_next(buf, sizeof(buf), &off, &nal, &nlen) == 1, "first nal");
    expect(nlen == 2 && nal[0] == 0x67 && nal[1] == 0xaa, "sps bytes");
    expect(annexb_next(buf, sizeof(buf), &off, &nal, &nlen) == 1, "second nal");
    expect(nlen == 2 && nal[0] == 0x68, "pps");
    expect(annexb_next(buf, sizeof(buf), &off, &nal, &nlen) == 0, "last waits for tail");
    expect(annexb_tail(buf + off, sizeof(buf) - off, &nal, &nlen) == 1, "tail");
    expect(nlen == 3 && nal[0] == 0x65 && annexb_is_vcl(nal, nlen), "idr");
}

struct cap {
    unsigned char pkt[8][1500];
    size_t len[8];
    int marker[8];
    int n;
};

static void on_rtp(void *user, const unsigned char *pkt, size_t len, int marker)
{
    struct cap *c = (struct cap *)user;
    if (c->n >= 8)
        return;
    if (len > sizeof(c->pkt[0]))
        len = sizeof(c->pkt[0]);
    memcpy(c->pkt[c->n], pkt, len);
    c->len[c->n] = len;
    c->marker[c->n] = marker;
    c->n++;
}

static void test_rtp(void)
{
    struct rtp_state st;
    struct cap cap;
    unsigned char small[20];
    unsigned char big[2000];
    unsigned char rebuilt[2000];
    size_t got = 0;
    int i;

    memset(&st, 0, sizeof(st));
    st.pt = 96;
    st.ssrc = 0x11223344;
    st.seq = 7;
    memset(small, 0x11, sizeof(small));
    small[0] = 0x65;
    memset(&cap, 0, sizeof(cap));
    rtp_h264_pack(&st, small, sizeof(small), 3600, 1, on_rtp, &cap);
    expect(cap.n == 1, "single packet");
    expect(cap.marker[0] == 1, "single marker");
    expect(cap.pkt[0][0] == 0x80, "rtp v2");
    expect(cap.pkt[0][1] == (unsigned char)(0x80 | 96), "marker and pt");
    expect(cap.len[0] == 12 + sizeof(small), "single size");
    expect(memcmp(cap.pkt[0] + 12, small, sizeof(small)) == 0, "single payload");
    expect(st.seq == 8, "seq stepped");

    memset(big, 0x22, sizeof(big));
    big[0] = 0x65;
    st.seq = 1;
    memset(&cap, 0, sizeof(cap));
    rtp_h264_pack(&st, big, sizeof(big), 7200, 1, on_rtp, &cap);
    expect(cap.n >= 2, "fu-a split");
    expect(cap.marker[cap.n - 1] == 1, "fu-a end marker");
    expect(cap.marker[0] == 0, "fu-a start not marker");
    expect((cap.pkt[0][12] & 0x1f) == 28, "fu indicator");
    expect((cap.pkt[0][13] & 0x80) != 0, "fu start bit");
    expect((cap.pkt[cap.n - 1][13] & 0x40) != 0, "fu end bit");
    rebuilt[0] = (unsigned char)((cap.pkt[0][12] & 0xe0) | (cap.pkt[0][13] & 0x1f));
    got = 1;
    for (i = 0; i < cap.n; i++) {
        size_t plen = cap.len[i] - 12 - 2;
        unsigned seq = ((unsigned)cap.pkt[i][2] << 8) | cap.pkt[i][3];
        expect(seq == (unsigned)(1 + i), "fu seq");
        memcpy(rebuilt + got, cap.pkt[i] + 14, plen);
        got += plen;
    }
    expect(got == sizeof(big), "fu rebuilt length");
    expect(memcmp(rebuilt, big, sizeof(big)) == 0, "fu rebuilt bytes");
}

static void test_sdp_and_transport(void)
{
    char sdp[512];
    char udp[128];
    char name[64];

    expect(sdp_write_h264(sdp, sizeof(sdp)) > 0, "sdp");
    expect(strstr(sdp, "a=rtpmap:96 H264/90000") != NULL, "rtpmap");
    expect(strstr(sdp, "a=control:trackID=0") != NULL, "control");
    expect(rtsp_format_udp_transport(udp, sizeof(udp), 16000, 16001, 30000, 30001) == 0, "udp fmt");
    expect(strstr(udp, "server_port=30000-30001") != NULL, "server_port hyphen");
    rtsp_stream_name("rtsp://127.0.0.1:18554/live/trackID=0", name, sizeof(name));
    expect(strcmp(name, "live") == 0, "stream name");
}

int main(void)
{
    test_annexb();
    test_rtp();
    test_sdp_and_transport();
    if (g_fail) {
        fprintf(stderr, "%d checks failed\n", g_fail);
        return 1;
    }
    printf("unit ok\n");
    return 0;
}
