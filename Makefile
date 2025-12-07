CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g
LIBS = -lpthread -lncurses

SRC = src/main.c src/mural.c src/tedax.c src/ui.c src/coordinator.o
OBJ = $(SRC:.c=.o)
TARGET = ksne

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
