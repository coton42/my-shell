CC = cc
CFLAGS = -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L
TARGET = my-shell
OBJS = main.o parse.o exec.o

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

main.o: main.c parse.h exec.h shell.h
	$(CC) $(CFLAGS) -c main.c

parse.o: parse.c parse.h shell.h
	$(CC) $(CFLAGS) -c parse.c

exec.o: exec.c exec.h shell.h
	$(CC) $(CFLAGS) -c exec.c

clean:
	rm -f $(TARGET) $(OBJS)
