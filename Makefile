CC = gcc
CFLAGS = -Wall -Wextra -std=gnu11 -g -Iinclude -I/usr/include/x86_64-linux-gnu -I/usr/include/postgresql -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lm -lffi -ldl -lcurl -lpq -lcrypto -lssl -lsqlite3 -lhiredis -lpthread -luring

SRCDIR = src
INCDIR = include
BUILDDIR = build

SRCS = $(wildcard $(SRCDIR)/*.c)
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
release: CFLAGS += -O2 -DNDEBUG -D_FORTIFY_SOURCE=2 -fstack-protector-strong \
	-fstack-clash-protection -fPIE -fno-strict-aliasing -Wformat -Wformat-security
release: LDFLAGS += -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack
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
