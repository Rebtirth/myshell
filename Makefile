CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -g
PYTHON ?= python3

TARGET = myshell
OBJECTS = main.o parser.o executor.o

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJECTS) $(LDLIBS) -o $@

main.o: main.c parser.h executor.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

parser.o: parser.c parser.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

executor.o: executor.c executor.h parser.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test: $(TARGET)
	$(PYTHON) tests/test_shell.py "$(abspath $(TARGET))"

clean:
	$(RM) $(TARGET) $(OBJECTS)
