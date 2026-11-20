#ifndef RTSPS_IO_H
#define RTSPS_IO_H

#include <stddef.h>

int io_set_nonblock(int fd);
int io_write_all(int fd, const void *buf, size_t len, int (*on_block)(void *user), void *user);

#endif
