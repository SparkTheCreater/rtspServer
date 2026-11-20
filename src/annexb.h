#ifndef RTSPS_ANNEXB_H
#define RTSPS_ANNEXB_H

#include <stddef.h>
#include <stdint.h>

int annexb_is_vcl(const uint8_t *nal, size_t len);

/* 从 offset 起找下一对起始码之间的 NALU。找到后 offset 停在下一个起始码。 */
int annexb_next(const uint8_t *buf, size_t len, size_t *offset,
                const uint8_t **nal, size_t *nal_len);

/* 缓冲区末尾、后面不再有起始码的那一个 NALU。 */
int annexb_tail(const uint8_t *buf, size_t len, const uint8_t **nal, size_t *nal_len);

#endif
