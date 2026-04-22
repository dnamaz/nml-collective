include ../../platform.mk

EDGE    := ../../edge
LIB     := $(EDGE)/libcollective.a
CC      ?= gcc
CFLAGS  := -std=c99 -O2 -Wall -Wextra -I$(EDGE) -I$(EDGE)/mqtt
TARGET  := enforcer_agent$(PLATFORM_EXT)

# ui.html.h embeds the landing page so the binary ships with zero per-deploy
# config. Generated symbols are `ui_html[]` and `ui_html_len`.
$(TARGET): enforcer_agent.c ui.html.h $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB) -lm $(PLATFORM_LIBS)

ui.html.h: ui.html
	$(EMBED_CMD) ui.html ui.html.h

$(LIB):
	$(MAKE) -C $(EDGE) libcollective.a

clean:
	rm -f $(TARGET) ui.html.h

.PHONY: clean
