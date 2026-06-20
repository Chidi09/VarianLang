CC = gcc
CFLAGS = -Wall -Wextra -std=gnu11 -g -Iinclude -I/usr/include/x86_64-linux-gnu -I/usr/include/postgresql -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lm -lffi -ldl -lcurl -lpq -lcrypto -lssl -lsqlite3 -lhiredis -lpthread -luring

SRCDIR = src
INCDIR = include
BUILDDIR = build

SRCS = $(wildcard $(SRCDIR)/*.c)
OBJS = $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(SRCS))
TARGET = vn

.PHONY: all clean test run

all: $(BUILDDIR) $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(INCDIR)/*.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test: $(TARGET)
	@echo "Running lexer/parser tests..."
	@./$(TARGET) examples/test.vn 2>&1 || true

run: $(TARGET)
	@echo "Varian REPL (type 'exit' to quit)"
	@./$(TARGET)

debug: CFLAGS += -DVN_DEBUG -g3 -O0
debug: $(TARGET)

clean:
	rm -rf $(BUILDDIR) $(TARGET)

# Example test files
.PHONY: examples
examples: $(TARGET)
	@for f in examples/*.vn; do \
		echo "=== Running $$f ==="; \
		VN_DEBUG_AST=1 VN_DEBUG_BYTECODE=1 ./$(TARGET) $$f 2>&1; \
		echo ""; \
	done
