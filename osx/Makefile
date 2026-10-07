CC = g++ 
CFLAGS	:= -O3 -std=c++14

SDL_CFLAG := $(shell sdl2-config --cflags | sed 's/\/SDL2//')
SDL_LFLAG := $(shell sdl2-config --libs)

CFLAGS += -pthread

SRC := ../src
OBJS := objmemory.o bitblt.o main.o interpreter.o evaluation.o hostservices.o hostbridge.o

Smalltalk: $(OBJS) 
	$(CC) -F/Library/Frameworks $(SDL_LFLAG)  -pthread -o $@  $^

main.o: $(SRC)/main.cpp $(wildcard $(SRC)/*.h)
	$(CC) $(CFLAGS) $(SDL_CFLAG) -F/Library/Frameworks   -c $(SRC)/main.cpp

%.o: $(SRC)/%.cpp $(wildcard $(SRC)/*.h)
	$(CC) $(CFLAGS) -c $<  -o $@


clean:
	rm -f *.o Smalltalk

