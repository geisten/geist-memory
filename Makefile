.DEFAULT_GOAL := all
include mk/config.mk
export ZERO_AR_DATE := 1

CORE := src/gm.c src/gm_store.c src/gm_format.c src/gm_hash.c src/gm_platform.c
ADAPTER := src/embedder_geist.c
OBJECTS := $(CORE:src/%.c=$(BUILD)/%.o)
ADAPTER_OBJECT := $(ADAPTER:src/%.c=$(BUILD)/%.o)
TEST_OBJECTS := $(CORE:src/%.c=$(BUILD)/test/%.o)
DEPS := $(OBJECTS:.o=.d) $(ADAPTER_OBJECT:.o=.d) $(TEST_OBJECTS:.o=.d)

.PHONY: all lib test test-unit test-store test-e2e check check-headers analyze \
        engine check-engine adapter help print-config clean install check-install check-linkage bench fuzz
all lib: $(LIB)
$(LIB): $(OBJECTS)
	$(AR) rcs $@ $(OBJECTS)
	$(RANLIB) $@
$(BUILD)/%.o: src/%.c Makefile mk/config.mk
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -Iinclude -Isrc -MMD -MP -c $< -o $@
# The adapter compiles like an external embedder: public headers and geistlib only.
adapter: $(ADAPTER_LIB)
$(ADAPTER_LIB): $(ADAPTER_OBJECT)
	$(AR) rcs $@ $<
	$(RANLIB) $@
$(ADAPTER_OBJECT): $(ADAPTER) Makefile mk/config.mk | check-engine
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -Iinclude -I$(ENGINE_SRC)/include -MMD -MP -c $< -o $@
$(BUILD)/test/%.o: src/%.c Makefile mk/config.mk
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -DGM_TESTING -Iinclude -Isrc -MMD -MP -c $< -o $@
$(BUILD)/test_core: test/test_core.c test/mock_embedder.c test/test_support.c $(TEST_OBJECTS)
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -DGM_TESTING -Iinclude -Isrc -Itest $^ $(LDFLAGS) $(SAN_FLAGS) $(LINK_FLAGS) $(PROJECT_LIBS) -o $@
test: test-unit test-store test-quality test-format test-import
test-unit: $(BUILD)/test_core
	$(BUILD)/test_core
$(BUILD)/test_store: test/test_store.c test/test_support.c $(BUILD)/test/gm_store.o $(BUILD)/test/gm_format.o $(BUILD)/test/gm_hash.o $(BUILD)/test/gm_platform.o
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -DGM_TESTING -Iinclude -Isrc -Itest $^ $(LDFLAGS) $(SAN_FLAGS) $(LINK_FLAGS) $(PROJECT_LIBS) -o $@
test-store: $(BUILD)/test_store
	$(BUILD)/test_store
check: test check-headers
check-headers:
	@mkdir -p $(BUILD)
	printf '#include "geist_memory.h"\nint main(void) { return 0; }\n' | $(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -Iinclude -x c - -o $(BUILD)/header-c $(SAN_FLAGS)
	printf '#include "geist_memory.h"\nint main() { return 0; }\n' | $(CXX) -std=c++17 -Wall -Wextra -pedantic-errors -Iinclude -x c++ - -o $(BUILD)/header-cxx
	printf '#include "geist_memory_embedder.h"\nint main(void) { return 0; }\n' | $(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -Iinclude -x c - -o $(BUILD)/embedder-c $(SAN_FLAGS)
	printf '#include "geist_memory_embedder.h"\nint main() { return 0; }\n' | $(CXX) -std=c++17 -Wall -Wextra -pedantic-errors -Iinclude -x c++ - -o $(BUILD)/embedder-cxx
analyze:
	@mkdir -p $(BUILD)/analysis
	@for source in $(CORE) test/quality.c test/bench_model.c; do $(CC) --analyze -Werror -std=c23 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE -Iinclude -Isrc -Itest -Xanalyzer -analyzer-werror -Xanalyzer -analyzer-output=text $$source -o $(BUILD)/analysis/$$(basename $$source).plist || exit; done
$(ENGINE_SRC)/.ready: $(ENGINE_PATCH)
	@mkdir -p $(ENGINE_SRC)
	git -C $(GEISTLIB) archive --format=tar --output=$(ENGINE_SRC)/source.tar $(GEIST_REV)
	tar -xf $(ENGINE_SRC)/source.tar -C $(ENGINE_SRC)
	rm $(ENGINE_SRC)/source.tar
	cd $(ENGINE_SRC) && patch -p1 < $(abspath $(ENGINE_PATCH))
	@touch $@
check-engine: $(ENGINE_SRC)/.ready
engine: $(ENGINE_LIB)
.PHONY: FORCE
FORCE:
$(ENGINE_LIB): FORCE | check-engine
	$(MAKE) -C $(ENGINE_SRC) lib TARGET=$(ENGINE_TARGET) MODE=$(MODE) CC='$(CC)' AR='$(AR)' \
	    BACKENDS='$(BACKENDS)' GEMM_PROVIDER=native CFLAGS_TARGET='$(ENGINE_FLAGS)' \
	    EXTRA_CFLAGS='$(CPPFLAGS) $(CFLAGS)' LDFLAGS_TARGET= LDLIBS_TARGET=-lm BUILD_DIR='$(abspath $(BUILD)/engine/obj)' \
	    LIB_DIR='$(abspath $(BUILD)/engine)' BIN_DIR='$(abspath $(BUILD)/engine/bin)'
$(BUILD)/test_gm_e2e: test/test_gm_e2e.c test/test_support.c $(GEIST_LIBS)
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -Iinclude -Isrc -Itest $< test/test_support.c $(GEIST_LIBS) $(LDFLAGS) $(SAN_FLAGS) $(LINK_FLAGS) $(ENGINE_LINK_LIBS) -o $@
test-e2e: check-model $(BUILD)/test_gm_e2e
	GEIST_EMBED_GGUF_PATH='$(GEIST_EMBED_GGUF_PATH)' $(BUILD)/test_gm_e2e
print-config:
	@printf '%s\n' 'TARGET=$(TARGET)' 'CC=$(CC)' 'MODE=$(MODE)' 'LINK=$(LINK)' 'BACKENDS=$(BACKENDS)' 'GEMM_PROVIDER=$(GEMM_PROVIDER)' 'GEIST_REV=$(GEIST_REV)' 'ENGINE_PATCH_ID=$(ENGINE_PATCH_ID)' 'BUILD=$(BUILD)'
help:
	@printf '%s\n' 'make [lib]          static geist-memory archive' 'make check          model-free tests + C/C++ headers' \
	 'make MODE=asan check ASan + UBSan tests' 'make analyze        Clang static analysis' \
	 'make adapter        geistlib embedder archive; make engine builds the pinned engine' 'make test-e2e       requires GEIST_EMBED_GGUF_PATH' \
	 'make check-linkage check-install  external consumer and staged installation' \
	 'make fuzz           deterministic corrupt-file tests (MODE=asan recommended)' \
	 'make fuzz-libfuzzer coverage-guided Clang fuzzing' 'make bench          store latency and memory' 'make bench-model    real-model DE/EN quality and latency' \
	 'make check-package  package normalization and failure tests' 'make check-repro    two fresh builds and package comparison' \
	 'make import-tool    non-destructive v1 format import CLI' \
	 'make example        minimal remember/recall CLI' 'make release-check  includes mandatory real-model E2E' \
	 'make install        PREFIX and DESTDIR supported' 'make print-config   resolved configuration' \
	 'TARGET=darwin-arm64|darwin-x86_64|linux-x86_64|linux-aarch64|pi5' \
	 'MODE=release|debug|asan LINK=system|static (Linux release only)'
clean:
	rm -rf $(BUILD)

-include $(DEPS)

$(BUILD)/consumer: test/test_consumer.c $(GEIST_LIBS)
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -Iinclude $< $(GEIST_LIBS) $(LDFLAGS) $(SAN_FLAGS) $(LINK_FLAGS) $(ENGINE_LINK_LIBS) -o $@
$(BUILD)/memory: examples/memory.c $(GEIST_LIBS)
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -Iinclude $< $(GEIST_LIBS) $(LDFLAGS) $(SAN_FLAGS) $(LINK_FLAGS) $(ENGINE_LINK_LIBS) -o $@
.PHONY: example
example: $(BUILD)/memory
install: $(GEIST_LIBS)
	install -d '$(DESTDIR)$(PREFIX)/include' '$(DESTDIR)$(PREFIX)/lib/pkgconfig' '$(DESTDIR)$(PREFIX)/share/doc/geist-memory/docs' '$(DESTDIR)$(PREFIX)/share/doc/geist-memory/patches'
	install -m 644 LICENSE README.md PLAN.md CHANGELOG.md CONTRIBUTING.md '$(DESTDIR)$(PREFIX)/share/doc/geist-memory/'
	install -m 644 docs/*.md '$(DESTDIR)$(PREFIX)/share/doc/geist-memory/docs/'
	install -m 644 patches/*.patch patches/*.md '$(DESTDIR)$(PREFIX)/share/doc/geist-memory/patches/'
	install -m 644 $(ENGINE_SRC)/LICENSE '$(DESTDIR)$(PREFIX)/share/doc/geist-memory/GEIST-LICENSE'
	install -m 644 $(ENGINE_SRC)/NOTICE '$(DESTDIR)$(PREFIX)/share/doc/geist-memory/GEIST-NOTICE'
	install -m 644 include/geist_memory.h include/geist_memory_embedder.h '$(DESTDIR)$(PREFIX)/include/'
	install -m 644 $(GEIST_LIBS) '$(DESTDIR)$(PREFIX)/lib/'
	printf '%s\n' 'prefix=$(PREFIX)' 'libdir=$${prefix}/lib' 'includedir=$${prefix}/include' \
	    '' 'Name: geist-memory' 'Description: local semantic memory in C23; link an embedder' 'Version: 0.1.0' \
	    'Libs: -L$${libdir} -lgeist_memory' 'Libs.private: -lm' 'Cflags: -I$${includedir}' \
	    > '$(DESTDIR)$(PREFIX)/lib/pkgconfig/geist-memory.pc'
	printf '%s\n' 'prefix=$(PREFIX)' 'libdir=$${prefix}/lib' 'includedir=$${prefix}/include' \
	    '' 'Name: geist-memory-geist' 'Description: geist-memory with the bundled geistlib embedder' 'Version: 0.1.0' \
	    'Libs: -L$${libdir} -lgeist_memory -lgeist_memory_geist' 'Libs.private: -lgeist $(ENGINE_SYSTEM_LIBS)' 'Cflags: -I$${includedir}' \
	    > '$(DESTDIR)$(PREFIX)/lib/pkgconfig/geist-memory-geist.pc'
# The staged consumer links through the installed .pc; that also checks its archive order.
check-install: $(GEIST_LIBS)
	$(MAKE) install PREFIX=/usr DESTDIR='$(abspath $(BUILD)/stage)'
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) test/test_consumer.c \
	    $$(PKG_CONFIG_SYSROOT_DIR='$(abspath $(BUILD)/stage)' PKG_CONFIG_LIBDIR='$(abspath $(BUILD)/stage)/usr/lib/pkgconfig' \
	       $(PKG_CONFIG) --static --cflags --libs geist-memory-geist) \
	    $(LDFLAGS) $(SAN_FLAGS) $(LINK_FLAGS) $(LDLIBS) -o $(BUILD)/installed-consumer
	$(BUILD)/installed-consumer
# Core without geistlib: no geist_ reference, and complete with a foreign embedder.
# Adapter: no core reference, so the archive order core, adapter, engine has no cycle.
check-linkage: $(BUILD)/consumer $(BUILD)/consumer-mock
	$(BUILD)/consumer
	$(BUILD)/consumer-mock
	sh tools/check-linkage.sh $(BUILD)/consumer $(LINK)
	sh test/test_linkage.sh
	nm -u $(LIB) > $(BUILD)/core-undefined && ! grep -E '(^|[[:space:]])_?geist_' $(BUILD)/core-undefined
	nm -u $(ADAPTER_LIB) > $(BUILD)/adapter-undefined && ! grep -E '(^|[[:space:]])_?gm_' $(BUILD)/adapter-undefined
$(BUILD)/consumer-mock: test/test_consumer.c test/mock_embedder.c test/test_support.c $(LIB)
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -DGM_TESTING -Iinclude -Isrc -Itest $^ $(LDFLAGS) $(SAN_FLAGS) $(LINK_FLAGS) $(PROJECT_LIBS) -o $@

FUZZ_RUNS ?= 3000
FUZZ_SECONDS ?= 30
FUZZ_CC ?= clang
$(BUILD)/fuzz-store: test/fuzz_store.c test/test_support.c $(BUILD)/test/gm_store.o $(BUILD)/test/gm_format.o $(BUILD)/test/gm_hash.o $(BUILD)/test/gm_platform.o
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -DGM_TESTING -DGM_FUZZ_STANDALONE -Iinclude -Isrc -Itest $^ $(LDFLAGS) $(SAN_FLAGS) $(PROJECT_LIBS) -o $@
fuzz: $(BUILD)/fuzz-store
	$(BUILD)/fuzz-store $(FUZZ_RUNS)
.PHONY: fuzz-libfuzzer
fuzz-libfuzzer: $(BUILD)/fuzz-store
	@mkdir -p $(BUILD)/corpus
	$(BUILD)/fuzz-store --seed > $(BUILD)/corpus/seed
	$(FUZZ_CC) $(CPPFLAGS) -std=c23 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE \
	    -O1 -g -fsanitize=fuzzer,address,undefined -DGM_TESTING -Iinclude -Isrc -Itest \
	    test/fuzz_store.c test/test_support.c src/gm_store.c src/gm_format.c src/gm_platform.c src/gm_hash.c \
	    -o $(BUILD)/fuzz-libfuzzer
	$(BUILD)/fuzz-libfuzzer $(BUILD)/corpus -max_total_time=$(FUZZ_SECONDS) -max_len=65536 -rss_limit_mb=512

BENCH_CHUNKS ?= 100000
$(BUILD)/bench-store: test/bench_store.c test/test_support.c $(BUILD)/test/gm_store.o $(BUILD)/test/gm_format.o $(BUILD)/test/gm_hash.o $(BUILD)/test/gm_platform.o
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -DGM_TESTING -Iinclude -Isrc -Itest $^ $(LDFLAGS) $(SAN_FLAGS) $(PROJECT_LIBS) -o $@
bench: $(BUILD)/bench-store
	$(BUILD)/bench-store $(BENCH_CHUNKS)
CLANG_FORMAT ?= clang-format
.PHONY: format format-check release-check
format:
	$(CLANG_FORMAT) -i src/*.c src/*.h include/*.h test/*.c test/*.h examples/*.c
format-check:
	$(CLANG_FORMAT) --dry-run --Werror src/*.c src/*.h include/*.h test/*.c test/*.h examples/*.c
release-check: import-tool check format-check check-linkage check-install check-package fuzz test-e2e bench-model

# Packaging creates a reviewable local artifact; it does not publish a release.
.PHONY: dist
dist: $(GEIST_LIBS)
	@set -eu; stage=$$(mktemp -d '$(abspath $(BUILD))/package.XXXXXX'); \
	trap 'rm -rf "$$stage"' EXIT HUP INT TERM; \
	$(MAKE) install PREFIX=/usr DESTDIR="$$stage" && \
	$(MAKE) --no-print-directory print-config > "$$stage/BUILD.txt" && \
	$(CC) --version >> "$$stage/BUILD.txt" && \
	printf '%s\n' 'CPPFLAGS=$(CPPFLAGS)' 'CFLAGS=$(CFLAGS)' 'LDFLAGS=$(LDFLAGS)' 'LDLIBS=$(LDLIBS)' >> "$$stage/BUILD.txt" && \
	sh tools/package.sh "$$stage" '$(abspath $(BUILD))/geist-memory-$(TARGET).tar.gz'

.PHONY: check-repro
check-repro:
	@test '$(MODE)' = release || { echo 'check-repro requires MODE=release'; exit 1; }
	+sh tools/check-repro.sh '$(abspath $(GEISTLIB))' '$(MAKE)'

.PHONY: bench-model test-quality
$(BUILD)/test-quality: test/test_quality.c test/quality.c test/quality.h test/test_support.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -Iinclude -Isrc -Itest test/test_quality.c test/quality.c $(LDFLAGS) $(SAN_FLAGS) $(LINK_FLAGS) $(PROJECT_LIBS) -o $@
$(BUILD)/bench-model: test/bench_model.c test/quality.c test/quality.h test/retrieval_cases.h $(GEIST_LIBS)
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -Iinclude -Isrc -Itest test/bench_model.c test/quality.c $(GEIST_LIBS) $(LDFLAGS) $(SAN_FLAGS) $(LINK_FLAGS) $(ENGINE_LINK_LIBS) -o $@
$(BUILD)/bench-model-mock: test/bench_model.c test/quality.c test/quality.h test/retrieval_cases.h test/mock_embedder.c test/test_support.c $(TEST_OBJECTS)
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -DGM_TESTING -DGM_MODEL_MOCK -Iinclude -Isrc -Itest test/bench_model.c test/quality.c test/mock_embedder.c test/test_support.c $(TEST_OBJECTS) $(LDFLAGS) $(SAN_FLAGS) $(LINK_FLAGS) $(PROJECT_LIBS) -o $@
test-quality: $(BUILD)/test-quality $(BUILD)/bench-model-mock
	$(BUILD)/test-quality
	@printf 'model harness fixture\n' > $(BUILD)/model-fixture
	GM_QUERY_PREFIX='query: ' GM_OMIT_BOS=0 GM_OMIT_EOS=0 GEIST_EMBED_GGUF_PATH=$(BUILD)/model-fixture $(BUILD)/bench-model-mock > $(BUILD)/quality-mock.txt
	@grep -q '^model_source=mock' $(BUILD)/quality-mock.txt
	@grep -q '^language=all queries=16 ' $(BUILD)/quality-mock.txt
bench-model: check-model $(BUILD)/bench-model
	@$(MAKE) --no-print-directory print-config
	@$(CC) --version | head -1
	GEIST_EMBED_GGUF_PATH='$(GEIST_EMBED_GGUF_PATH)' $(BUILD)/bench-model

.PHONY: check-model model-tools check-package
check-model:
	@test -n "$(GEIST_EMBED_GGUF_PATH)" && test -f "$(GEIST_EMBED_GGUF_PATH)" || { echo 'GEIST_EMBED_GGUF_PATH must name an existing model'; exit 1; }
model-tools: $(BUILD)/bench-model $(BUILD)/test_gm_e2e
check-package:
	sh test/test_package.sh

.PHONY: import-tool
$(BUILD)/memory-import-v1: examples/import_v1.c $(GEIST_LIBS)
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -Iinclude $< $(GEIST_LIBS) $(LDFLAGS) $(SAN_FLAGS) $(LINK_FLAGS) $(ENGINE_LINK_LIBS) -o $@
import-tool: $(BUILD)/memory-import-v1

.PHONY: test-format
$(BUILD)/test-format: test/test_format.c test/fixtures/v2_64.h test/test_support.c $(BUILD)/test/gm_store.o $(BUILD)/test/gm_format.o $(BUILD)/test/gm_hash.o $(BUILD)/test/gm_platform.o
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -DGM_TESTING -Iinclude -Isrc -Itest test/test_format.c test/test_support.c $(BUILD)/test/gm_store.o $(BUILD)/test/gm_format.o $(BUILD)/test/gm_hash.o $(BUILD)/test/gm_platform.o $(LDFLAGS) $(SAN_FLAGS) $(LINK_FLAGS) $(PROJECT_LIBS) -o $@
test-format: $(BUILD)/test-format
	$(BUILD)/test-format

.PHONY: test-import
$(BUILD)/test-import: test/test_import.c test/test_support.c $(BUILD)/test/gm_store.o $(BUILD)/test/gm_format.o $(BUILD)/test/gm_hash.o $(BUILD)/test/gm_platform.o
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -DGM_TESTING -Iinclude -Isrc -Itest $^ $(LDFLAGS) $(SAN_FLAGS) $(LINK_FLAGS) $(PROJECT_LIBS) -o $@
test-import: $(BUILD)/test-import
	$(BUILD)/test-import

# ENOSPC_DIR must name a dedicated disposable filesystem <=64 MiB.
.PHONY: test-enospc
$(BUILD)/test-enospc: test/test_enospc.c test/test_support.c $(BUILD)/test/gm_store.o $(BUILD)/test/gm_format.o $(BUILD)/test/gm_hash.o $(BUILD)/test/gm_platform.o
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -DGM_TESTING -Iinclude -Isrc -Itest $^ $(LDFLAGS) $(SAN_FLAGS) $(LINK_FLAGS) $(PROJECT_LIBS) -o $@
test-enospc: $(BUILD)/test-enospc
	@test -n '$(ENOSPC_DIR)' || { echo 'ENOSPC_DIR must name a dedicated disposable filesystem <=64 MiB'; exit 1; }
	$(BUILD)/test-enospc '$(ENOSPC_DIR)'

# Optional model setup, separate from the dependency-free model-free test suite.
PYTHON ?= python3
BITNET_MODEL ?= build/models/bitnet-embedding-0.6b-geist.gguf
.PHONY: prepare-model
prepare-model:
	@test -n '$(BITNET_SOURCE)' && test -f '$(BITNET_SOURCE)' || { echo 'BITNET_SOURCE must name the official 0.6B GGUF'; exit 1; }
	@mkdir -p '$(dir $(BITNET_MODEL))'
	$(PYTHON) tools/prepare-bitnet.py '$(BITNET_SOURCE)' '$(BITNET_MODEL)'

# External, generated fixture; never a dependency of the model-free build.
QUALITY_HEADER ?= build/quality/scifact.h
.PHONY: bench-model-large
bench-model-large: check-model $(BUILD)/bench-model-large
	@$(MAKE) --no-print-directory print-config
	GM_TRUNCATE=1 GM_OMIT_BOS=1 GM_OMIT_EOS=0 GEIST_EMBED_GGUF_PATH='$(GEIST_EMBED_GGUF_PATH)' $(BUILD)/bench-model-large
$(BUILD)/bench-model-large: test/bench_model.c test/quality.c test/quality.h $(QUALITY_HEADER) $(GEIST_LIBS)
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -DGM_RETRIEVAL_HEADER='"$(abspath $(QUALITY_HEADER))"' -Iinclude -Isrc -Itest test/bench_model.c test/quality.c $(GEIST_LIBS) $(LDFLAGS) $(SAN_FLAGS) $(LINK_FLAGS) $(ENGINE_LINK_LIBS) -o $@

# GNU ld test instrumentation; production objects and allocator remain unchanged.
WRAP_ALLOC := malloc calloc realloc aligned_alloc posix_memalign strdup free
.PHONY: model-memory
model-memory: check-model $(if $(findstring linux,$(COMPILER_TARGET)),$(BUILD)/test-model-memory)
	@test '$(findstring linux,$(COMPILER_TARGET))' = linux || { echo 'model-memory requires Linux/GNU ld'; exit 1; }
	GEIST_EMBED_GGUF_PATH='$(GEIST_EMBED_GGUF_PATH)' $(BUILD)/test-model-memory $(FAULT_PHASE) $(FAULT_AT) $(FAULT_MIN)
FAULT_PHASE ?= none
FAULT_AT ?= -1
FAULT_MIN ?= 0
$(BUILD)/test-model-memory: test/test_model_memory.c test/model_alloc.c test/model_alloc.h test/test_support.c $(GEIST_LIBS)
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -Iinclude -Isrc -Itest test/test_model_memory.c test/model_alloc.c test/test_support.c $(GEIST_LIBS) $(foreach name,$(WRAP_ALLOC),-Wl,--wrap=$(name)) $(LDFLAGS) $(SAN_FLAGS) $(ENGINE_LINK_LIBS) -o $@
.PHONY: test-model-alloc
test-model-alloc: $(BUILD)/test-model-alloc
	$(BUILD)/test-model-alloc
$(BUILD)/test-model-alloc: test/test_model_alloc.c test/model_alloc.c test/model_alloc.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -fno-builtin -Iinclude -Isrc -Itest test/test_model_alloc.c test/model_alloc.c $(foreach name,$(WRAP_ALLOC),-Wl,--wrap=$(name)) $(LDFLAGS) $(SAN_FLAGS) -o $@
.PHONY: test-model-equivalence
test-model-equivalence: check-model $(BUILD)/test-model-equivalence
	GEIST_EMBED_GGUF_PATH='$(GEIST_EMBED_GGUF_PATH)' $(BUILD)/test-model-equivalence
$(BUILD)/test-model-equivalence: test/test_model_equivalence.c $(ENGINE_LIB)
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -Iinclude -Isrc -Itest -I$(ENGINE_SRC)/include $< $(ENGINE_LIB) $(LDFLAGS) $(SAN_FLAGS) $(ENGINE_LINK_LIBS) -o $@
.PHONY: test-tokenizer-oom
test-tokenizer-oom: $(BUILD)/test-tokenizer-oom
	$(BUILD)/test-tokenizer-oom
$(BUILD)/test-tokenizer-oom: test/test_tokenizer_oom.c test/model_alloc.c test/model_alloc.h $(ENGINE_LIB)
	$(CC) $(CPPFLAGS) $(BASE_FLAGS) $(CFLAGS) -Iinclude -Isrc -Itest -I$(ENGINE_SRC)/src/engine -I$(ENGINE_SRC)/src/io test/test_tokenizer_oom.c test/model_alloc.c $(ENGINE_LIB) $(foreach name,$(WRAP_ALLOC),-Wl,--wrap=$(name)) $(LDFLAGS) $(SAN_FLAGS) $(ENGINE_LINK_LIBS) -o $@
