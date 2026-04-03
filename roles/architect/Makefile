include ../../platform.mk

EDGE    := ../../edge
LIB     := $(EDGE)/libcollective.a
CC      ?= gcc
CFLAGS  := -std=c99 -O2 -Wall -Wextra -I$(EDGE) -I$(EDGE)/mqtt
TARGET  := architect_agent$(PLATFORM_EXT)

$(TARGET): architect_agent.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB) -lm $(PLATFORM_LIBS)

$(LIB):
	$(MAKE) -C $(EDGE) libcollective.a

clean:
	rm -f $(TARGET)

.PHONY: clean
