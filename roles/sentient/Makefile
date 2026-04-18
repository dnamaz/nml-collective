include ../../platform.mk

EDGE    := ../../edge
LIB     := $(EDGE)/libcollective.a
CC      ?= gcc
CFLAGS  := -std=c99 -O2 -Wall -Wextra -I$(EDGE) -I$(EDGE)/mqtt
TARGET  := sentient_agent$(PLATFORM_EXT)

$(TARGET): sentient_agent.c ui.html.h $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB) -lm $(PLATFORM_LIBS)

ui.html.h: ui.html
	xxd -i ui.html > ui.html.h

$(LIB):
	$(MAKE) -C $(EDGE) libcollective.a

clean:
	rm -f $(TARGET) ui.html.h

.PHONY: clean
