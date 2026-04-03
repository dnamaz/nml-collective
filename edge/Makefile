# NML Edge — Base library Makefile
#
# Targets:
#   make                   Build libcollective.a (default)
#   make edge_worker       Build standalone edge_worker binary (reference/legacy)
#   make arm               Cross-compile for ARM Cortex-M4 → libnml_edge_arm_m4.a
#   make clean             Remove build artifacts
#
# Role agents are built from roles/<role>/Makefile — each links libcollective.a.
#
# Overrides (via environment or command line):
#   CC=arm-none-eabi-gcc         Cross-compile for ARM bare metal
#   NML_MAX_TENSOR_SIZE=65536    Restrict tensor size for MCU (default: 1M)
#   EDGE_AGENT_NAME='"edge_2"'   Override agent name at compile time
#
# ARM toolchain requirement:
#   brew install --cask gcc-arm-embedded   (interactive — needs sudo)
#   or: brew install arm-none-eabi-gcc     (Homebrew, freestanding only)
#
# With gcc-arm-embedded (full newlib):
#   make arm ARM_CC=arm-none-eabi-gcc ARM_NEWLIB=1

include ../platform.mk

CC      ?= gcc
TARGET  := edge_worker$(PLATFORM_EXT)
NML_SRC := ../../nml/runtime

# Conservative tensor size: large enough for fraud detection (5×6=30 elements)
# but well within a Cortex-M4 RAM budget when cross-compiling.
NML_MAX_TENSOR_SIZE ?= 1048576

CFLAGS := -std=c99 -O2 -Wall -Wextra \
          -D_GNU_SOURCE \
          -DNML_MAX_TENSOR_SIZE=$(NML_MAX_TENSOR_SIZE) \
          -I. -Imqtt

# Suppress warnings from vendored code (NML runtime, MQTT-C)
NML_CFLAGS := $(CFLAGS) -w

# Optional agent name override
ifdef EDGE_AGENT_NAME
CFLAGS += -DEDGE_AGENT_NAME='"$(EDGE_AGENT_NAME)"'
endif

# ── Source files ─────────────────────────────────────────────────────────

EDGE_SRCS := main.c udp.c msg.c crypto.c report.c identity.c
EDGE_OBJS := $(EDGE_SRCS:.c=.o)

# nml_exec.c includes nml.c directly — NML_LIBRARY_MODE excludes main()
NML_EXEC_OBJ := nml_exec.o

# tweetnacl.c provides Ed25519 (extern symbols used by crypto.c)
TWEETNACL_OBJ := $(NML_SRC)/tweetnacl.o

ALL_OBJS := $(EDGE_OBJS) $(NML_EXEC_OBJ) $(TWEETNACL_OBJ)

# ── libcollective.a — shared library for all C collective role agents ──────
#
# Includes everything from the edge worker except main.c, plus the new shared
# modules (peer_table, vote, program_send).  Role binaries link against this.

COLLECTIVE_SRCS := udp.c msg.c crypto.c report.c identity.c \
                   peer_table.c vote.c program_send.c http_client.c \
                   templates.c mqtt_transport.c storage.c
COLLECTIVE_OBJS := $(COLLECTIVE_SRCS:.c=.o)

# Vendored MQTT-C library (MIT license)
MQTT_OBJS := mqtt/mqtt.o mqtt/mqtt_pal.o

# libcollective_all_objs includes the NML exec object, tweetnacl, and MQTT-C
COLLECTIVE_ALL_OBJS := $(COLLECTIVE_OBJS) $(MQTT_OBJS) $(NML_EXEC_OBJ) $(TWEETNACL_OBJ)

# ── Rules ─────────────────────────────────────────────────────────────────

.PHONY: all arm clean edge_worker

all: libcollective.a

edge_worker: $(ALL_OBJS)
	$(CC) -o $@ $^ -lm $(PLATFORM_LIBS)

# ── libcollective.a ────────────────────────────────────────────────────────

libcollective.a: $(COLLECTIVE_ALL_OBJS)
	ar rcs $@ $^
	@echo "Built: $@ (shared library for C collective agents)"

# program_send.c: includes nml_crypto.h — suppress unused-function warnings
program_send.o: program_send.c program_send.h msg.h udp.h config.h
	$(CC) $(CFLAGS) -Wno-unused-function -c -o $@ $<

# peer_table and vote have no special flags
peer_table.o: peer_table.c peer_table.h
vote.o: vote.c vote.h

# nml_exec.c: include nml.c as a library; suppress vendor warnings with -w
nml_exec.o: nml_exec.c nml_exec.h config.h
	$(CC) $(CFLAGS) -DNML_LIBRARY_MODE -w -c -o $@ $<

# tweetnacl: suppress all warnings (third-party)
$(TWEETNACL_OBJ): $(NML_SRC)/tweetnacl.c $(NML_SRC)/tweetnacl.h
	$(CC) $(NML_CFLAGS) -c -o $@ $<

# crypto.c: suppress unused-function warnings from vendored nml_crypto.h
crypto.o: crypto.c crypto.h
	$(CC) $(CFLAGS) -Wno-unused-function -c -o $@ $<

# identity.c: suppress unused-function warnings from vendored nml_crypto.h
identity.o: identity.c identity.h config.h
	$(CC) $(CFLAGS) -Wno-unused-function -c -o $@ $<

# All other edge sources
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Header dependencies
main.o:    main.c    config.h msg.h udp.h crypto.h nml_exec.h report.h identity.h
udp.o:     udp.c     udp.h
msg.o:     msg.c     msg.h config.h
crypto.o:  crypto.c  crypto.h
report.o:  report.c  report.h msg.h config.h

# ── ARM Cortex-M4 static library ──────────────────────────────────────────

ARM_CC      ?= arm-none-eabi-gcc
ARM_AR      ?= arm-none-eabi-ar
ARM_TARGET  := libnml_edge_arm_m4.a
ARM_PLAT    := platform/arm_m4

# Cortex-M4F flags (hard-float ABI, Thumb-2, FPv4-SP)
ARM_CPU_FLAGS := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

# When ARM_NEWLIB=1 (gcc-arm-embedded with newlib-nano installed) the
# platform stub headers are not needed — the toolchain provides its own.
ifdef ARM_NEWLIB
ARM_INC  := -I.
ARM_DEFS :=
else
ARM_INC  := -I. -I$(ARM_PLAT)/include
ARM_DEFS := -D_POSIX_C_SOURCE=0
endif

ARM_CFLAGS := -std=c99 -O2 -Wall -Wextra \
              -ffreestanding -nostdlib \
              $(ARM_CPU_FLAGS) \
              -DNML_MAX_TENSOR_SIZE=$(NML_MAX_TENSOR_SIZE) \
              $(ARM_INC) $(ARM_DEFS)

ifdef EDGE_AGENT_NAME
ARM_CFLAGS += -DEDGE_AGENT_NAME='"$(EDGE_AGENT_NAME)"'
endif

# Platform stubs (only needed when NOT using newlib)
ifndef ARM_NEWLIB
ARM_PLAT_SRCS := $(ARM_PLAT)/alloc.c \
                 $(ARM_PLAT)/printf.c \
                 $(ARM_PLAT)/string_stubs.c \
                 $(ARM_PLAT)/time_stubs.c \
                 $(ARM_PLAT)/udp.c \
                 $(ARM_PLAT)/posix_stubs.c
ARM_PLAT_OBJS := $(ARM_PLAT_SRCS:.c=.arm.o)
else
ARM_PLAT_OBJS :=
endif

# Core edge sources (entry.c replaces main.c; udp.c replaced by platform/arm_m4/udp.c)
ARM_EDGE_SRCS := msg.c crypto.c report.c nml_exec.c identity.c
ARM_ENTRY_SRC := $(ARM_PLAT)/entry.c

ARM_EDGE_OBJS  := $(ARM_EDGE_SRCS:.c=.arm.o)
ARM_ENTRY_OBJ  := $(ARM_PLAT)/entry.arm.o
ARM_NACL_OBJ   := $(NML_SRC)/tweetnacl.arm.o

ARM_ALL_OBJS := $(ARM_EDGE_OBJS) $(ARM_ENTRY_OBJ) $(ARM_NACL_OBJ) $(ARM_PLAT_OBJS)

arm: $(ARM_TARGET)

$(ARM_TARGET): $(ARM_ALL_OBJS)
	$(ARM_AR) rcs $@ $^
	@echo "Built: $@ (static library for ARM Cortex-M4)"
	@echo "Link with: -L. -lnml_edge_arm_m4 $(ARM_CPU_FLAGS) -lm"

# Generic ARM compile rule (%.arm.o from %.c)
%.arm.o: %.c
	$(ARM_CC) $(ARM_CFLAGS) -c -o $@ $<

# nml_exec.arm.o: NML_LIBRARY_MODE + vendor-warning suppression
nml_exec.arm.o: nml_exec.c nml_exec.h config.h
	$(ARM_CC) $(ARM_CFLAGS) -DNML_LIBRARY_MODE -w -c -o $@ $<

# crypto.arm.o: suppress unused-function from nml_crypto.h
crypto.arm.o: crypto.c crypto.h
	$(ARM_CC) $(ARM_CFLAGS) -Wno-unused-function -c -o $@ $<

# identity.arm.o
identity.arm.o: identity.c identity.h config.h
	$(ARM_CC) $(ARM_CFLAGS) -Wno-unused-function -c -o $@ $<

# tweetnacl (third-party)
$(NML_SRC)/tweetnacl.arm.o: $(NML_SRC)/tweetnacl.c $(NML_SRC)/tweetnacl.h
	$(ARM_CC) $(ARM_CFLAGS) -w -c -o $@ $<

# Platform stubs
$(ARM_PLAT)/alloc.arm.o: $(ARM_PLAT)/alloc.c
	$(ARM_CC) $(ARM_CFLAGS) -w -c -o $@ $<

$(ARM_PLAT)/printf.arm.o: $(ARM_PLAT)/printf.c
	$(ARM_CC) $(ARM_CFLAGS) -w -c -o $@ $<

$(ARM_PLAT)/string_stubs.arm.o: $(ARM_PLAT)/string_stubs.c
	$(ARM_CC) $(ARM_CFLAGS) -w -c -o $@ $<

$(ARM_PLAT)/time_stubs.arm.o: $(ARM_PLAT)/time_stubs.c
	$(ARM_CC) $(ARM_CFLAGS) -w -c -o $@ $<

$(ARM_PLAT)/udp.arm.o: $(ARM_PLAT)/udp.c
	$(ARM_CC) $(ARM_CFLAGS) -w -c -o $@ $<

$(ARM_PLAT)/posix_stubs.arm.o: $(ARM_PLAT)/posix_stubs.c
	$(ARM_CC) $(ARM_CFLAGS) -w -c -o $@ $<

$(ARM_PLAT)/entry.arm.o: $(ARM_PLAT)/entry.c
	$(ARM_CC) $(ARM_CFLAGS) -w -c -o $@ $<

# mqtt_transport.c: uses MQTT-C library
mqtt_transport.o: mqtt_transport.c mqtt_transport.h mqtt/mqtt.h msg.h
	$(CC) $(CFLAGS) -c -o $@ $<

# storage.c: uses nml_crypto.h for SHA-256
storage.o: storage.c storage.h
	$(CC) $(CFLAGS) -Wno-unused-function -c -o $@ $<

# Vendored MQTT-C (suppress all warnings)
mqtt/mqtt.o: mqtt/mqtt.c mqtt/mqtt.h mqtt/mqtt_pal.h
	$(CC) $(NML_CFLAGS) -c -o $@ $<

mqtt/mqtt_pal.o: mqtt/mqtt_pal.c mqtt/mqtt_pal.h
	$(CC) $(NML_CFLAGS) -c -o $@ $<

clean:
	rm -f $(EDGE_OBJS) $(NML_EXEC_OBJ) $(TWEETNACL_OBJ) edge_worker
	rm -f $(COLLECTIVE_OBJS) $(MQTT_OBJS) libcollective.a
	rm -f $(ARM_ALL_OBJS) $(ARM_TARGET)
