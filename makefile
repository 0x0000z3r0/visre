CC = gcc
LIBS = -lGL -lm -lpthread -ldl -lrt -lX11 -lraylib
SAN = -fsanitize=leak,address,undefined

all:
	$(CC) main.c -o visre $(LIBS)
