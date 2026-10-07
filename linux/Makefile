CFLAGS	:=  -std=c++17 -O3

CC = g++ 
CFLAGS += -pthread

SRC := ../src
OBJS := objmemory.o bitblt.o main.o interpreter.o evaluation.o hostservices.o hostbridge.o

Smalltalk: $(OBJS) 
	$(CC) -pthread -o $@ $^ -L/usr/local/lib -lSDL2main -lSDL2
	
main.o: $(SRC)/main.cpp $(wildcard $(SRC)/*.h)
	$(CC) $(CFLAGS) -c $(SRC)/main.cpp

%.o: $(SRC)/%.cpp $(wildcard $(SRC)/*.h)
	$(CC) $(CFLAGS) -c $<  -o $@


clean:
	rm -f *.o Smalltalk

