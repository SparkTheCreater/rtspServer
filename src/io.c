#include "io.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>

int io_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

int io_write_all(int fd, const void *buf, size_t len, int (*on_block)(void *user), void *user)
{
    const unsigned char *p = (const unsigned char *)buf;
    size_t off = 0;

    if (fd < 0 || (!buf && len > 0))
        return -1;
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            if (on_block && on_block(user) < 0)
                return -1;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            n = poll(&pfd, 1, 1000);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return -1;
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}
