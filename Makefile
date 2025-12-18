CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -pedantic -g -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lcrypto

SHAM_SRC = sham.c
CLIENT_SRC = client.c
SERVER_SRC = server.c

SHAM_OBJ = sham.o
CLIENT_OBJ = client.o
SERVER_OBJ = server.o

CLIENT_EXE = client
SERVER_EXE = server

RM = rm -f

all: $(CLIENT_EXE) $(SERVER_EXE)

$(CLIENT_EXE): $(CLIENT_OBJ) $(SHAM_OBJ)
	$(CC) $(CLIENT_OBJ) $(SHAM_OBJ) -o $(CLIENT_EXE) $(LDFLAGS)

$(SERVER_EXE): $(SERVER_OBJ) $(SHAM_OBJ)
	$(CC) $(SERVER_OBJ) $(SHAM_OBJ) -o $(SERVER_EXE) $(LDFLAGS)

$(SHAM_OBJ): $(SHAM_SRC) sham.h
	$(CC) $(CFLAGS) -c $(SHAM_SRC) -o $(SHAM_OBJ)

$(CLIENT_OBJ): $(CLIENT_SRC) sham.h
	$(CC) $(CFLAGS) -c $(CLIENT_SRC) -o $(CLIENT_OBJ)

$(SERVER_OBJ): $(SERVER_SRC) sham.h
	$(CC) $(CFLAGS) -c $(SERVER_SRC) -o $(SERVER_OBJ)

clean:
	$(RM) *.o $(CLIENT_EXE) $(SERVER_EXE) *_log.txt

.PHONY: all clean
