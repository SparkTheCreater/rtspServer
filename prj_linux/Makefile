TOP_DIR := ..
PLATFORM ?= linux64
CROSS_COMPILE ?=

.PHONY: all clean lib demo

all: lib demo

lib:
	$(MAKE) -C $(TOP_DIR)/src PLATFORM=$(PLATFORM) CROSS_COMPILE=$(CROSS_COMPILE)

demo: lib
	$(MAKE) -C $(TOP_DIR)/demo/linux64 PLATFORM=$(PLATFORM) CROSS_COMPILE=$(CROSS_COMPILE)

clean:
	$(MAKE) -C $(TOP_DIR)/src clean
	$(MAKE) -C $(TOP_DIR)/demo/linux64 clean
