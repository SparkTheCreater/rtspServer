#include "annexb.h"

static int start_at(const uint8_t *buf, size_t len, size_t i, int *sc)
{
    if (i + 4 <= len && buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1) {
        *sc = 4;
        return 1;
    }
    if (i + 3 <= len && buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
        *sc = 3;
        return 1;
    }
    return 0;
}

int annexb_is_vcl(const uint8_t *nal, size_t len)
{
    int t;

    if (!nal || len < 1)
        return 0;
    t = nal[0] & 0x1f;
    return t >= 1 && t <= 5;
}

int annexb_next(const uint8_t *buf, size_t len, size_t *offset,
                const uint8_t **nal, size_t *nal_len)
{
    size_t i;
    size_t start_pos = 0;
    int sc = 0;

    if (!buf || !offset || !nal || !nal_len)
        return 0;
    for (i = *offset; i < len; i++) {
        if (start_at(buf, len, i, &sc)) {
            start_pos = i;
            break;
        }
    }
    if (sc == 0)
        return 0;
    {
        size_t nal_off = start_pos + (size_t)sc;
        size_t j;
        for (j = nal_off; j < len; j++) {
            int sc2 = 0;
            if (start_at(buf, len, j, &sc2)) {
                *nal = buf + nal_off;
                *nal_len = j - nal_off;
                *offset = j;
                return 1;
            }
        }
    }
    return 0;
}

int annexb_tail(const uint8_t *buf, size_t len, const uint8_t **nal, size_t *nal_len)
{
    size_t i;

    if (!buf || !nal || !nal_len)
        return 0;
    for (i = 0; i < len; i++) {
        int sc = 0;
        if (!start_at(buf, len, i, &sc))
            continue;
        {
            size_t nal_off = i + (size_t)sc;
            size_t j;
            if (nal_off >= len)
                return 0;
            for (j = nal_off; j < len; j++) {
                int sc2 = 0;
                if (start_at(buf, len, j, &sc2))
                    return 0;
            }
            *nal = buf + nal_off;
            *nal_len = len - nal_off;
            return *nal_len > 0;
        }
    }
    return 0;
}
