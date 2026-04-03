# platform.mk — Shared platform detection for NML Collective Makefiles.
#
# Include from any Makefile:
#   include ../../platform.mk    (from roles/<role>/)
#   include ../platform.mk       (from edge/)
#   include platform.mk          (from root)
#
# Provides:
#   PLATFORM_LIBS  — extra linker flags (-lws2_32 etc.)
#   PLATFORM_EXT   — binary extension (.exe on Windows, empty otherwise)

UNAME_S := $(shell uname -s 2>/dev/null || echo unknown)

PLATFORM_LIBS :=
PLATFORM_EXT  :=

ifneq (,$(findstring MINGW,$(UNAME_S)))
  PLATFORM_LIBS := -lws2_32 -liphlpapi
  PLATFORM_EXT  := .exe
endif

ifneq (,$(findstring MSYS,$(UNAME_S)))
  PLATFORM_LIBS := -lws2_32 -liphlpapi
  PLATFORM_EXT  := .exe
endif
