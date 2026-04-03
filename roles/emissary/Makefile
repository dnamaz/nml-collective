include ../../platform.mk

EDGE    := ../../edge
LIB     := $(EDGE)/libcollective.a
CC      ?= gcc
CFLAGS  := -std=c99 -O2 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -I$(EDGE) -I$(EDGE)/mqtt
TARGET  := emissary_agent$(PLATFORM_EXT)

$(TARGET): emissary_agent.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB) -lm $(PLATFORM_LIBS)

$(LIB):
	$(MAKE) -C $(EDGE) libcollective.a

clean:
	rm -f $(TARGET)

.PHONY: clean
