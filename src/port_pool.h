#ifndef RTSPS_PORT_POOL_H
#define RTSPS_PORT_POOL_H

int port_pool_init(unsigned short min_port, unsigned short max_port);
void port_pool_fini(void);
int port_pool_alloc(unsigned short *rtp_port);
void port_pool_free(unsigned short rtp_port);

#endif
