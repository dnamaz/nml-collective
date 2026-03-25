EDGE    := ../../edge
LIB     := $(EDGE)/libcollective.a
CC      ?= gcc
CFLAGS  := -std=c99 -O2 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -I$(EDGE) -I$(EDGE)/mqtt
TARGET  := custodian_agent

$(TARGET): custodian_agent.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< $(LIB) -lm

$(LIB):
	$(MAKE) -C $(EDGE) libcollective.a

clean:
	rm -f $(TARGET)

.PHONY: clean
