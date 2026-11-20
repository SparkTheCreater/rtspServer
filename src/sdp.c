#include "sdp.h"

#include <stdio.h>

int sdp_write_h264(char *out, size_t cap)
{
    int n;

    if (!out || cap == 0)
        return -1;
    n = snprintf(out, cap,
                 "v=0\r\n"
                 "o=- 0 0 IN IP4 0.0.0.0\r\n"
                 "s=rtsps\r\n"
                 "c=IN IP4 0.0.0.0\r\n"
                 "t=0 0\r\n"
                 "a=control:*\r\n"
                 "m=video 0 RTP/AVP 96\r\n"
                 "a=rtpmap:96 H264/90000\r\n"
                 "a=fmtp:96 packetization-mode=1\r\n"
                 "a=control:trackID=0\r\n");
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return n;
}
