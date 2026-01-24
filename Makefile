CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lX11 -lXext -lXinerama -lXft

SRC = main.c
BIN = build/calwm

all: $(BIN)

$(BIN): $(SRC)
	mkdir -p build
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDFLAGS)

clean:
	rm -rf build
	rm -f *.o

.PHONY: all clean
