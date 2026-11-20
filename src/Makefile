TOP_DIR := ..
PLATFORM ?= linux64
BIN_DIR := $(TOP_DIR)/lib/$(PLATFORM)
CROSS_COMPILE ?=
CC := $(CROSS_COMPILE)gcc
CFLAGS := -std=c11 -D_GNU_SOURCE -Wall -Wextra -g -fPIC -pthread -I$(TOP_DIR)/include -I.
LDFLAGS := -shared -Wl,-Bsymbolic -pthread

SRCS := log.c io.c port_pool.c annexb.c filter.c sdp.c rtp_h264.c rtsp_msg.c stream.c session.c server.c
OBJS := $(SRCS:.c=.o)
TARGET := librtsps.so

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	mkdir -p $(BIN_DIR)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)
	cp $@ $(BIN_DIR)/
	rm -f $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
