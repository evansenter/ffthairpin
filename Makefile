# Makefile for FFThairpin

CFLAGS  = -c -O3
LDFLAGS = -lfftw3 -lm -L.
BINDIR  = /usr/local/bin # Change this to the BINDIR
CC      = g++

FFThairpin: delta.o misc.o main.o
	 $(CC) -g delta.o misc.o main.o $(LDFLAGS) -lRNA -o FFThairpin
	
main.o: main.cpp delta.h
	   $(CC) -Wall -W $(CFLAGS) main.cpp

misc.o: misc.cpp misc.h
	 $(CC) -Wall -W -g $(CFLAGS) misc.cpp

delta.o: delta.cpp delta.h params.h energy_par.h energy_const.h
	$(CC) -Wall -W -g $(CFLAGS) delta.cpp

clean:
	rm -f *.o FFThairpin

install:
	cp FFThairpin $(BINDIR)
	cp energy.par $(BINDIR)
