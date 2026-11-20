#ifndef RTSPS_RTP_H264_H
#define RTSPS_RTP_H264_H

#include <stddef.h>
#include <stdint.h>

struct rtp_state {
    uint16_t seq;
    uint32_t ssrc;
    uint8_t pt;
};

typedef void (*rtp_emit_fn)(void *user, const uint8_t *pkt, size_t len, int marker);

void rtp_h264_pack(struct rtp_state *st, const uint8_t *nal, size_t len,
                   uint32_t timestamp, int marker, rtp_emit_fn emit, void *user);

#endif
