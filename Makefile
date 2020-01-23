OPT_GCC = -std=c++17
#compiler options and libraries for Linux
OPT = 

all: client server

client: client.cpp
	g++ $(OPT_GCC) $(OPT) -g -o client client.cpp

server: server.cpp
	g++ $(OPT_GCC) $(OPT) -g -o server server.cpp
	./server reset

rserver: server
	./server reset
	./server 6001

lclient: client
	./client 127.0.0.1 6001 test.txt

r1: client
	./user1/client 18.191.105.23 6001 t1.1

r2: client
	./user2/client 18.191.105.23 6001 t2.2

r3: client
	./user3/client 18.191.105.23 6001 t5.3

clean: server
	rm -f client server