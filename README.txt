Multiple client - single server program
- Currently broken (work in progress)
- Extremely low-level code. Everything is built from the ground up, including tcp socket connections and reinventing async
- Goal is to have 20 concurrent clients that can perform 8 concurrent file transfers and unhindered msg transfering per client through a single server. 
- Functionality is similar to a primitive Slack
- Uses pollfd to monitor the numerous socket events and flag manipulation to detect and escape blocking to simulate asynchrous/concurrent processing. (Having numerous parallel threads in client and server programs significantly increases complexity and is not feasible for large numbers of concurrent clients and their respective data transfers)
- Probably very poorly designed, but it's a great learning experience.

To run the program:

make clean
- resets database and cleans executables

make rserver
- runs the server

make lclient
- runs client in localhost

make r1

make r2

make r3

make rclient
-runs the client
- looks for a test script txt file 
- default is test.txt but can be easily changed in makefile

Please modify MakeFile and .vscode configs to suit your local environment
