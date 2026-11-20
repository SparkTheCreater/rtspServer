PLATFORM ?= linux64
TOP_DIR := ../..
INC_DIR := -I$(TOP_DIR)/include
BIN_DIR := $(TOP_DIR)/lib/$(PLATFORM)
TARGET := demo
CROSS_COMPILE ?=
CC := $(CROSS_COMPILE)gcc
CFLAGS := -std=c11 -D_GNU_SOURCE -Wall -Wextra -g $(INC_DIR)
LDFLAGS := -L$(BIN_DIR) -lrtsps -pthread -Wl,-rpath,'$$ORIGIN'

.PHONY: all clean

all: $(TARGET)

$(TARGET): src/main.c
	mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(TARGET) src/main.c $(LDFLAGS)
	cp $(TARGET) $(BIN_DIR)/
	rm -f $(TARGET)

clean:
	rm -f $(TARGET) $(BIN_DIR)/$(TARGET)
