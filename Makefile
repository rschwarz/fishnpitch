CC = gcc

CCFLAGS = -O2 -c -Wall

LDFLAGS = -lm `pkg-config --cflags --libs jack`

SOURCES = fishnpitch.c

OBJECTS = $(SOURCES:.c=.o)


all: $(OBJECTS) 
	$(CC) -o fishnpitch $(OBJECTS) $(LDFLAGS)

clean:
	rm -f $(OBJECTS) fishnpitch

%.o: %.c
	$(CC) $(CCFLAGS) -c $<
