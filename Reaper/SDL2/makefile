ifeq ($(OS),Windows_NT)
    CC = gcc
    RM = del /Q
    EXE = .exe
    INCLUDE = -I./include
    LDFLAGS = -lmingw32 -lSDL2main -lSDL2 -lSDL2_image -lSDL2_net -lSDL2_ttf -lSDL2_mixer
    SERVER_LDFLAGS = -mconsole -lmingw32 -lSDL2main -lSDL2 -lSDL2_net
    LIBS =
else
    CC = gcc
    RM = rm -f
    EXE =
    INCLUDE = -I/opt/homebrew/include -I./include
    LIBS = -L/opt/homebrew/lib
    LDFLAGS = -lSDL2 -lSDL2_image -lSDL2_net -lSDL2_ttf -lSDL2_mixer
    SERVER_LDFLAGS = -lSDL2 -lSDL2_net
endif

SRCDIR = ./src
CFLAGS = -g $(INCLUDE)

all: game server

game:
	$(CC) $(CFLAGS) -c $(SRCDIR)/main.c   -o main.o
	$(CC) $(CFLAGS) -c $(SRCDIR)/player.c -o player.o
	$(CC) main.o player.o -o game$(EXE) $(LIBS) $(LDFLAGS)

server:
	$(CC) $(CFLAGS) -c $(SRCDIR)/server.c -o server.o
	$(CC) server.o -o server$(EXE) $(LIBS) $(SERVER_LDFLAGS)

clean:
	$(RM) main.o player.o server.o
	$(RM) game$(EXE)
	$(RM) server$(EXE)
