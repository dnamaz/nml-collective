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

# Portable `xxd -i` replacement. `xxd` is missing from some Windows make
# toolchains (e.g. chocolatey's make with Git Bash); Python is universally
# available, so we prefer a small helper script.
PLATFORM_MK_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
EMBED_CMD       := python "$(PLATFORM_MK_DIR)tools/embed_file.py"

ifneq (,$(findstring MINGW,$(UNAME_S)))
  PLATFORM_LIBS := -lws2_32 -liphlpapi
  PLATFORM_EXT  := .exe
endif

ifneq (,$(findstring MSYS,$(UNAME_S)))
  PLATFORM_LIBS := -lws2_32 -liphlpapi
  PLATFORM_EXT  := .exe
endif
