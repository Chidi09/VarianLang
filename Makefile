CC = gcc

# ── Platform detection ───────────────────────────────────────────────────────
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)
UNAME_O := $(shell uname -o 2>/dev/null || echo Windows)
ifeq ($(OS),Windows_NT)
    PLATFORM := Windows
else ifneq ($(filter Msys% Cygwin%,$(UNAME_O)),)
    PLATFORM := Windows
else
    PLATFORM := $(UNAME_S)
endif

# ── Optional native modules ──────────────────────────────────────────────────
# Defaults keep Linux feature-complete; macOS uses bottles; Windows is minimal.
ifeq ($(PLATFORM),Linux)
    USE_HTTP    ?= 1
    USE_POSTGRES?= 1
    USE_SQLITE  ?= 1
    USE_REDIS   ?= 1
    USE_FFI     ?= 1
else ifeq ($(PLATFORM),Darwin)
    USE_HTTP    ?= 1
    USE_POSTGRES?= 1
    USE_SQLITE  ?= 1
    USE_REDIS   ?= 1
    USE_FFI     ?= 1
else ifeq ($(PLATFORM),Windows)
    USE_HTTP    ?= 1
    USE_POSTGRES?= 1
    USE_SQLITE  ?= 1
    USE_REDIS   ?= 1
    USE_FFI     ?= 1
else
    USE_HTTP    ?= 0
    USE_POSTGRES?= 1
    USE_SQLITE  ?= 1
    USE_REDIS   ?= 1
    USE_FFI     ?= 1
endif

CFLAGS = -Wall -Wextra -std=gnu11 -g -Iinclude -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lm -lcurl -lcrypto -lssl -lpthread

ifeq ($(PLATFORM),Linux)
    CFLAGS  += -I/usr/include/x86_64-linux-gnu
    LDFLAGS += -ldl
endif
ifeq ($(PLATFORM),Darwin)
    CFLAGS  += -I/usr/local/include -I/opt/homebrew/include \
               -I/usr/local/opt/libffi/include -I/opt/homebrew/opt/libffi/include \
               -I/usr/local/opt/openssl/include -I/opt/homebrew/opt/openssl/include \
               -I/usr/local/opt/hiredis/include -I/opt/homebrew/opt/hiredis/include
    LDFLAGS += -L/usr/local/lib -L/opt/homebrew/lib \
               -L/usr/local/opt/libffi/lib -L/opt/homebrew/opt/libffi/lib \
               -L/usr/local/opt/openssl/lib -L/opt/homebrew/opt/openssl/lib \
               -L/usr/local/opt/hiredis/lib -L/opt/homebrew/opt/hiredis/lib \
               -framework CoreFoundation
endif
ifeq ($(PLATFORM),Windows)
    DEPS_DIR ?= C:/deps
    CFLAGS += -I$(DEPS_DIR)/include
    LDFLAGS += -L$(DEPS_DIR)/lib
    LDFLAGS += -Wl,-Bstatic -lsqlite3 -lhiredis -lffi -Wl,-Bdynamic -lws2_32 -lwinmm -lcrypt32
    CFLAGS += -D_WIN32_WINNT=0x0600
    # Static tre (regex) library - built from source, no DLL dependency
    TRE_LIB = deps/libtre.a
    CFLAGS += -DUSE_LOCAL_TRE_H -Ideps/tre-0.9.0/local_includes
    LDFLAGS += deps/libtre.a
    ifeq ($(USE_SQLITE),1)
        CFLAGS += -I$(DEPS_DIR)
        LDFLAGS += -L$(DEPS_DIR)
    endif
    ifeq ($(USE_REDIS),1)
        CFLAGS += -I$(DEPS_DIR)/hiredis/include
        LDFLAGS += -L$(DEPS_DIR)
    endif
    ifeq ($(USE_POSTGRES),1)
        CFLAGS  += -I$(DEPS_DIR)/pg/include
        LDFLAGS += -L$(DEPS_DIR)/pg/lib
    endif
    ifeq ($(USE_FFI),1)
        CFLAGS  += -I$(DEPS_DIR)/libffi/include
        LDFLAGS += -L$(DEPS_DIR)/libffi/lib
    endif
endif

ifeq ($(USE_HTTP),1)
    ifeq ($(PLATFORM),Linux)
        LDFLAGS += -luring
    endif
else
    CFLAGS += -DVN_NO_HTTP
endif

ifeq ($(USE_POSTGRES),1)
    ifeq ($(PLATFORM),Linux)
        CFLAGS += -I/usr/include/postgresql
    endif
    ifeq ($(PLATFORM),Darwin)
        CFLAGS  += -I/usr/local/opt/libpq/include -I/opt/homebrew/opt/libpq/include
        LDFLAGS += -L/usr/local/opt/libpq/lib -L/opt/homebrew/opt/libpq/lib
    endif
    LDFLAGS += -lpq
else
    CFLAGS += -DVN_NO_POSTGRES
endif

ifeq ($(USE_SQLITE),1)
    LDFLAGS += -lsqlite3
else
    CFLAGS += -DVN_NO_SQLITE
endif

ifeq ($(USE_REDIS),1)
    LDFLAGS += -lhiredis
else
    CFLAGS += -DVN_NO_REDIS
endif

ifeq ($(USE_FFI),1)
    LDFLAGS += -lffi
else
    CFLAGS += -DVN_NO_FFI
endif

ifneq ($(USE_SMTP),1)
    CFLAGS += -DVN_NO_SMTP
endif
ifeq ($(USE_SMTP),)
    USE_SMTP := 0
endif

# ── Source files ─────────────────────────────────────────────────────────────
SRCDIR = src
INCDIR = include
BUILDDIR = build

SRCS = $(wildcard $(SRCDIR)/*.c)
ifneq ($(filter Windows,$(PLATFORM)),)
    SRCS := $(filter-out $(SRCDIR)/main_win32.c, $(SRCS))
endif

ifeq ($(USE_HTTP),0)
    SRCS := $(filter-out $(SRCDIR)/lib_http.c, $(SRCS))
endif
ifeq ($(USE_POSTGRES),0)
    SRCS := $(filter-out $(SRCDIR)/lib_postgres.c, $(SRCS))
endif
ifeq ($(USE_SQLITE),0)
    SRCS := $(filter-out $(SRCDIR)/lib_sqlite.c, $(SRCS))
endif
ifeq ($(USE_REDIS),0)
    SRCS := $(filter-out $(SRCDIR)/lib_redis.c, $(SRCS))
endif
ifeq ($(USE_FFI),0)
    SRCS := $(filter-out $(SRCDIR)/ffi.c, $(SRCS))
endif
ifeq ($(USE_SMTP),0)
    SRCS := $(filter-out $(SRCDIR)/lib_smtp.c, $(SRCS))
endif

OBJS = $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(SRCS))
TARGET = vn
LIB_TARGET = libvarian.a
LIB_OBJS = $(filter-out $(BUILDDIR)/main.o, $(OBJS))

.PHONY: all clean test run deploy bundle

all: $(BUILDDIR) $(TARGET) $(LIB_TARGET) bundle

# Copy required DLLs alongside vn.exe on Windows (post-link)
# Build static tre regex library (Windows only; Linux/macOS use libc's regex)
ifeq ($(PLATFORM),Windows)
bundle: $(TARGET)
	@MINGW_BIN="$(DEPS_DIR)/bin"; \
	for dll in libwinpthread-1.dll libcurl-4.dll libcrypto-3-x64.dll \
	  libssl-3-x64.dll zlib1.dll libzstd.dll libbrotlicommon.dll \
	  libbrotlidec.dll libidn2-0.dll libiconv-2.dll libintl-8.dll \
	  libpsl-5.dll libssh2-1.dll libunistring-5.dll libnghttp2-14.dll \
	  libnghttp3-9.dll libngtcp2-16.dll libngtcp2_crypto_ossl-0.dll \
	  libgcc_s_seh-1.dll libstdc++-6.dll; do \
	  cp "$$MINGW_BIN/$$dll" . 2>/dev/null || true; \
	done
	@echo "Bundled runtime DLLs alongside vn.exe"
else
bundle:
	@true
endif

ifeq ($(PLATFORM),Windows)
deps/libtre.a:
	@echo "Building static tre library..."
	@TMP=/tmp TMPDIR=/tmp TEMP=/tmp PATH="$(PATH)" sh deps/build_tre.sh
else
deps/libtre.a:
	@true
endif

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(INCDIR)/*.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(TRE_LIB) $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(LIB_TARGET): $(LIB_OBJS)
	ar rcs $@ $^

test: $(TARGET)
	@echo "Running lexer/parser tests..."
	@./$(TARGET) examples/test.vn 2>&1 || true

run: $(TARGET)
	@echo "Varian REPL (type 'exit' to quit)"
	@./$(TARGET)

debug: CFLAGS += -DVN_DEBUG -g3 -O0
debug: $(TARGET)

# Hardened, optimized build for shipping. _FORTIFY_SOURCE needs -O1+, so it is
# only meaningful here (not on the default -g debug build). Stack canaries,
# full RELRO + immediate binding (no lazy-PLT overwrite), non-exec stack, and a
# position-independent executable for ASLR. This is the build to benchmark and
# to release.
ifeq ($(PLATFORM),Linux)
release: CFLAGS += -O2 -DNDEBUG -D_FORTIFY_SOURCE=2 -fstack-protector-strong \
	-fstack-clash-protection -fPIE -fno-strict-aliasing -Wformat -Wformat-security
release: LDFLAGS += -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack
else
release: CFLAGS += -O2 -DNDEBUG -fno-strict-aliasing
endif
# Clean must finish before the (parallel) rebuild starts, or `rm -rf build/`
# races the compiler under `make -j`. Two ordered sub-makes guarantee that; the
# inner one still parallelizes via the inherited jobserver.
release:
	$(MAKE) clean
	$(MAKE) $(TARGET) $(LIB_TARGET) CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)"

# AddressSanitizer + UBSan build for finding memory bugs under the test sweep
# and fuzzers. Slower; not for production. Run: make asan && ./vn test tests/
asan: CFLAGS += -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	-fno-sanitize-recover=all
asan: LDFLAGS += -fsanitize=address,undefined
asan: clean $(TARGET)

clean:
	rm -rf $(BUILDDIR) $(TARGET) $(LIB_TARGET)

# Bundle required runtime DLLs alongside the binary (Windows only)
ifeq ($(PLATFORM),Windows)
DEPLOY_DIR = deploy
MINGW_BIN = $(DEPS_DIR)/bin
dll_to_bundle = libwinpthread-1.dll libcurl-4.dll libcrypto-3-x64.dll \
	libssl-3-x64.dll zlib1.dll libzstd.dll libbrotlicommon.dll \
	libbrotlidec.dll libidn2-0.dll libiconv-2.dll libintl-8.dll \
	libpsl-5.dll libssh2-1.dll libunistring-5.dll libnghttp2-14.dll \
	libnghttp3-9.dll libngtcp2-16.dll libngtcp2_crypto_ossl-0.dll \
	libgcc_s_seh-1.dll libstdc++-6.dll
deploy: $(TARGET) $(LIB_TARGET)
	rm -rf $(DEPLOY_DIR)
	mkdir -p $(DEPLOY_DIR)
	cp $(TARGET) $(DEPLOY_DIR)/
	cp $(LIB_TARGET) $(DEPLOY_DIR)/
	cp -r include $(DEPLOY_DIR)/include
	cp -r vn_modules $(DEPLOY_DIR)/vn_modules
	cp README.md LICENSE $(DEPLOY_DIR)/ 2>/dev/null || true
	for dll in $(dll_to_bundle); do \
		cp "$(MINGW_BIN)/$$dll" $(DEPLOY_DIR)/ 2>/dev/null || true; \
	done
	@echo "Deployed to $(DEPLOY_DIR)/"
	@ls -la $(DEPLOY_DIR)/
else
deploy: $(TARGET)
	rm -rf $(DEPLOY_DIR)
	mkdir -p $(DEPLOY_DIR)
	cp $(TARGET) $(DEPLOY_DIR)/
	cp $(LIB_TARGET) $(DEPLOY_DIR)/
	cp -r include $(DEPLOY_DIR)/
	cp -r vn_modules $(DEPLOY_DIR)/
	cp README.md LICENSE $(DEPLOY_DIR)/ 2>/dev/null || true
	@echo "Deployed to $(DEPLOY_DIR)/"
endif

# Example test files
.PHONY: examples
examples: $(TARGET)
	@for f in examples/*.vn; do \
		echo "=== Running $$f ==="; \
		VN_DEBUG_AST=1 VN_DEBUG_BYTECODE=1 ./$(TARGET) $$f 2>&1; \
		echo ""; \
	done
