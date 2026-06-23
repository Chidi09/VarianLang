CC = gcc

# ── Platform detection ───────────────────────────────────────────────────────
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)
ifeq ($(OS),Windows_NT)
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
    LDFLAGS += -lws2_32 -lwinmm
    DEPS_DIR ?= C:/deps
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

# ── Source files ─────────────────────────────────────────────────────────────
SRCDIR = src
INCDIR = include
BUILDDIR = build

SRCS = $(wildcard $(SRCDIR)/*.c)

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

OBJS = $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(SRCS))
TARGET = vn
LIB_TARGET = libvarian.a
LIB_OBJS = $(filter-out $(BUILDDIR)/main.o, $(OBJS))

.PHONY: all clean test run

all: $(BUILDDIR) $(TARGET) $(LIB_TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(INCDIR)/*.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
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

# Example test files
.PHONY: examples
examples: $(TARGET)
	@for f in examples/*.vn; do \
		echo "=== Running $$f ==="; \
		VN_DEBUG_AST=1 VN_DEBUG_BYTECODE=1 ./$(TARGET) $$f 2>&1; \
		echo ""; \
	done
