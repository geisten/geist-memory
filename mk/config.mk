# GNU Make 3.81+; no network access or engine dependency for core tests.
MODE ?= release
GEISTLIB ?= ../geistlib
GEIST_REV := 32b432660948a50be05b355efa74a789456a37dd
ENGINE_PATCH := patches/geistlib-compat.patch
ENGINE_PATCH_ID := $(shell cksum $(ENGINE_PATCH) | cut -d' ' -f1)
TARGET ?= $(shell uname -s | tr A-Z a-z)-$(shell uname -m)
BACKENDS ?= cpu_scalar
GEMM_PROVIDER ?= native
LINK ?= system
PREFIX ?= /usr/local
AR ?= ar
RANLIB ?= ranlib
CXX ?= c++

ifeq ($(filter $(TARGET),darwin-arm64 darwin-x86_64 linux-x86_64 linux-aarch64 pi5),)
$(error Unsupported TARGET=$(TARGET))
endif
ifeq ($(filter $(MODE),release debug asan),)
$(error Unsupported MODE=$(MODE))
endif
ifneq ($(GEMM_PROVIDER),native)
$(error This minimal build supports GEMM_PROVIDER=native only)
endif
ifeq ($(filter $(LINK),system static),)
$(error LINK must be system or static)
endif
ifneq ($(filter-out cpu_scalar cpu_neon cpu_x86,$(BACKENDS)),)
$(error Unsupported BACKENDS=$(BACKENDS))
endif
ifeq ($(strip $(BACKENDS)),)
$(error BACKENDS cannot be empty)
endif
COMPILER_TARGET := $(shell $(CC) $(CFLAGS) -dumpmachine 2>/dev/null)
ifneq ($(filter darwin-%,$(TARGET)),)
ifeq ($(findstring darwin,$(COMPILER_TARGET)),)
$(error TARGET=$(TARGET) requires a macOS compiler; got $(COMPILER_TARGET))
endif
else
ifeq ($(findstring linux,$(COMPILER_TARGET)),)
$(error TARGET=$(TARGET) requires a Linux compiler; set CC to a cross compiler)
endif
endif
ifneq ($(filter %x86_64,$(TARGET)),)
ifeq ($(filter x86_64%,$(COMPILER_TARGET)),)
$(error TARGET=$(TARGET) requires an x86-64 compiler)
endif
else
ifeq ($(filter arm64% aarch64%,$(COMPILER_TARGET)),)
$(error TARGET=$(TARGET) requires an ARM64 compiler)
endif
endif
ifeq ($(LINK),static)
ifneq ($(filter darwin-%,$(TARGET)),)
$(error LINK=static is a Linux profile; macOS uses system libraries)
endif
ifneq ($(MODE),release)
$(error Static sanitizer/debug distributions are not supported; use MODE=release)
endif
LINK_FLAGS := -static
endif
ifeq ($(MODE),release)
MODE_FLAGS := -O2 -DNDEBUG
else ifeq ($(MODE),debug)
MODE_FLAGS := -O0 -g3
else
MODE_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
SAN_FLAGS := -fsanitize=address,undefined
endif
WARN := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes -Werror
BASE_FLAGS := -std=c23 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64 $(WARN) $(MODE_FLAGS)
ENGINE_TARGET := $(if $(filter darwin-%,$(TARGET)),mac,$(if $(filter pi5,$(TARGET)),pi5,linux))
ENGINE_FLAGS := -D_GNU_SOURCE -Wno-unknown-pragmas
# The pinned engine compares static-array parameters against nullptr. Keep
# its existing GCC warning exceptions scoped to the dependency only.
ifeq ($(findstring clang,$(shell $(CC) --version | head -1)),)
ENGINE_FLAGS += -Wno-nonnull-compare -Wno-vla-parameter
endif
ifeq ($(TARGET),pi5)
ENGINE_FLAGS += -DGEIST_TARGET_PI5=1 -mcpu=cortex-a76
endif
PROJECT_LIBS := -lm $(LDLIBS)
ENGINE_SYSTEM_LIBS := -lm
ifneq ($(filter darwin-%,$(TARGET)),)
ENGINE_SYSTEM_LIBS += -framework Accelerate
endif
ENGINE_LINK_LIBS := $(ENGINE_SYSTEM_LIBS) $(LDLIBS)

# Content-derived build directories prevent stale objects on configuration changes.
CONFIG := $(shell printf '%s\n' '$(CC)|$(shell $(CC) --version | head -1)|$(TARGET)|$(MODE)|$(CPPFLAGS)|$(CFLAGS)|$(LDFLAGS)|$(LDLIBS)|$(BACKENDS)|$(GEMM_PROVIDER)|$(LINK)|$(GEIST_REV)|$(ENGINE_PATCH_ID)|$(ENGINE_FLAGS)|$(AR)|$(RANLIB)|$(COMPILER_TARGET)|$(shell cksum Makefile mk/config.mk)' | cksum | cut -d' ' -f1)
BUILD := build/$(TARGET)/$(MODE)/$(CONFIG)
LIB := $(BUILD)/libgeist_memory.a
ENGINE_SRC := $(abspath build/engine-source/$(GEIST_REV)-$(ENGINE_PATCH_ID))
ENGINE_LIB := $(abspath $(BUILD)/engine/libgeist.a)
