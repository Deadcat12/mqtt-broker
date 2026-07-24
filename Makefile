CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O0
LDFLAGS ?=
TARGET = sol-broker
SRC = src/main.c src/mqtt.c src/broker.c
OBJ = build/main.o build/mqtt.o build/broker.o

.PHONY: all clean run
all: $(TARGET)

build:
	mkdir -p build

$(TARGET): build $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -I src -c $< -o $@

run: $(TARGET)
	./$(TARGET) 0.0.0.0 1883

clean:
	rm -rf build $(TARGET)
