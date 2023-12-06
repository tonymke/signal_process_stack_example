.PHONY: all check clean

CFLAGS = -std=c99 -g -Og -Wall -Wextra -Werror -pedantic -pedantic-errors

SRC = main.c
BIN = signal_process_stack_example

RM ?= rm -f  # not defined in POSIX make

all: $(BIN)

check: $(BIN)
	./$(BIN) $(CHECKFLAGS)

clean:
	$(RM) $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS) $(LDLIBS)
