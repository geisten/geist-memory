# geist-memory — local semantic memory on the geist engine.
#
# Mirrors geist-diktat: geistlib's own mk/ fragments decide the platform
# flags, so this library links with exactly what the engine was built with
# and no platform knowledge is duplicated here.
#
#   make                 # build lib/libgeist_memory.a
#   make test            # build + run the E2E test (needs GEIST_EMBED_GGUF_PATH)
#
# GEISTLIB points at a geistlib checkout. It becomes a pinned submodule once
# geist_session_embed lands in a geistlib release — pinning an API that is
# still EXPERIMENTAL and uncommitted would pin a moving target.

GEISTLIB ?= ../geistlib
TARGET   ?= $(shell $(GEISTLIB)/mk/detect-target.sh)
MODE     ?= release

include $(GEISTLIB)/mk/target-$(TARGET).mk

GEMM_PROVIDER ?= native
include $(GEISTLIB)/mk/gemm-$(GEMM_PROVIDER).mk

GEIST_LIB := $(GEISTLIB)/lib/$(TARGET)/$(MODE)/libgeist.a

CFLAGS  := -std=c23 -O2 -Wall -Wextra -Wpedantic -Wshadow \
           -Iinclude -Isrc -I$(GEISTLIB)/include $(CFLAGS_TARGET) $(GEMM_CFLAGS)
LDFLAGS := $(LDFLAGS_TARGET)
LDLIBS  := $(LDLIBS_TARGET) $(GEMM_LDLIBS)

BUILD := build/$(TARGET)/$(MODE)
LIB   := lib/$(TARGET)/$(MODE)/libgeist_memory.a
SRCS  := src/gm.c src/gm_store.c
OBJS  := $(SRCS:%.c=$(BUILD)/%.o)

.PHONY: all test clean
all: $(LIB)

$(LIB): $(OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(OBJS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(GEIST_LIB):
	$(MAKE) -C $(GEISTLIB) MODE=$(MODE)

test: $(BUILD)/test_gm_e2e
	@./$<

$(BUILD)/test_gm_e2e: test/test_gm_e2e.c $(LIB) $(GEIST_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $< $(LIB) $(GEIST_LIB) $(LDFLAGS) $(LDLIBS)

clean:
	rm -rf build lib

-include $(OBJS:.o=.d)
