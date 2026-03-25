# NML Collective — Root Makefile
#
# Targets:
#   make           Build libcollective.a then all role agents
#   make edge      Build libcollective.a only
#   make roles     Build all role agents (requires edge first)
#   make clean     Clean all build artifacts
#
# Pass-through overrides to sub-makes:
#   CC=gcc  NML_MAX_TENSOR_SIZE=65536  EDGE_AGENT_NAME='"node_1"'

ROLES := worker sentient oracle architect enforcer custodian herald emissary

.PHONY: all edge roles clean $(ROLES)

all: edge roles

edge:
	$(MAKE) -C edge

roles: edge $(ROLES)

$(ROLES):
	$(MAKE) -C roles/$@

clean:
	$(MAKE) -C edge clean
	for r in $(ROLES); do $(MAKE) -C roles/$$r clean; done
