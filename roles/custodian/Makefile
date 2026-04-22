include ../../platform.mk

EDGE    := ../../edge
LIB     := $(EDGE)/libcollective.a
CC      ?= gcc
CFLAGS  := -std=c99 -O2 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -I$(EDGE) -I$(EDGE)/mqtt
TARGET  := custodian_agent$(PLATFORM_EXT)

$(TARGET): custodian_agent.c ui.html.h $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB) -lm $(PLATFORM_LIBS)

ui.html.h: ui.html
	$(EMBED_CMD) ui.html ui.html.h

$(LIB):
	$(MAKE) -C $(EDGE) libcollective.a

clean:
	rm -f $(TARGET) ui.html.h

.PHONY: clean
