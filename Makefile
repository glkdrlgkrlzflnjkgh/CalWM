CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lX11 -lXext -lXinerama -lXft

SRC = main.c
OBJ = $(SRC:.c=.o)
BIN = calwm

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $(BIN) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(BIN)
