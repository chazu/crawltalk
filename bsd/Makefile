CFLAGS := -std=c++17 -O3 -I/usr/local/include

CC = clang++
CFLAGS += -pthread

SRC := ../src
OBJS := objmemory.o bitblt.o main.o interpreter.o evaluation.o hostservices.o hostbridge.o

Smalltalk: $(OBJS)
	$(CC) -pthread -o $@ $(OBJS) -L/usr/local/lib -lSDL2main -lSDL2

%.o: $(SRC)/%.cpp $(wildcard $(SRC)/*.h)
	$(CC) $(CFLAGS) -c $< -o $@


clean:
	rm -f *.o Smalltalk

