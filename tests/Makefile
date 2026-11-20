TOP := ..
CC := gcc
CFLAGS := -std=c11 -D_GNU_SOURCE -Wall -Wextra -g -I$(TOP)/src -I$(TOP)/include
UNIT_SRC := check.c $(TOP)/src/annexb.c $(TOP)/src/rtp_h264.c $(TOP)/src/sdp.c \
	$(TOP)/src/rtsp_msg.c $(TOP)/src/io.c $(TOP)/src/log.c

.PHONY: all clean

all: check

check: $(UNIT_SRC)
	$(CC) $(CFLAGS) -o $@ $(UNIT_SRC)

clean:
	rm -f check
