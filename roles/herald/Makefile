include ../../platform.mk

CC      ?= gcc
CFLAGS  := -std=c99 -O2 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -I../../edge
TARGET  := herald_agent$(PLATFORM_EXT)

# Herald doesn't link libcollective.a (it's infrastructure, not a mesh agent),
# but it does share the HTTP helpers with the other roles — compile + link
# edge/http_util.c directly.
HTTP_UTIL := ../../edge/http_util.o

$(TARGET): herald_agent.c ui.html.h $(HTTP_UTIL)
	$(CC) $(CFLAGS) -o $@ $< $(HTTP_UTIL) $(PLATFORM_LIBS)

$(HTTP_UTIL): ../../edge/http_util.c ../../edge/http_util.h
	$(MAKE) -C ../../edge http_util.o

ui.html.h: ui.html
	$(EMBED_CMD) ui.html ui.html.h

clean:
	rm -f $(TARGET) ui.html.h

.PHONY: clean
