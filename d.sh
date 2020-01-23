cp client.cpp ./user1/client.cpp
cp client.cpp ./user2/client.cpp
cp client.cpp ./user3/client.cpp
g++ ./user1/client.cpp -g -o ./user1/client
g++ ./user2/client.cpp -g -o ./user2/client
g++ ./user3/client.cpp -g -o ./user3/client