PROJECT = load_gen
CFLAGS  = -g -Wall -pedantic -I./libevent-2.0.4-alpha/include -I./libevent-2.0.4-alpha
OBJECTS = load_gen.o
LIBS = ./libevent-2.0.4-alpha/.libs/libevent.a 

all: $(PROJECT)

.c.o:
	gcc -c $(CFLAGS) $<

$(PROJECT): $(OBJECTS)
	gcc $(OBJECTS) $(LIBS) -lrt -o $(PROJECT)

clean:
	rm -rf $(OBJECTS) $(PROJECT)
