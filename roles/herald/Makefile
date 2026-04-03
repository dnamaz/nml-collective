include ../../platform.mk

CC      ?= gcc
CFLAGS  := -std=c99 -O2 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -I../../edge
TARGET  := herald_agent$(PLATFORM_EXT)

$(TARGET): herald_agent.c
	$(CC) $(CFLAGS) -o $@ $< $(PLATFORM_LIBS)

clean:
	rm -f $(TARGET)

.PHONY: clean
