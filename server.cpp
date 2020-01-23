//Rolling alphabetical table
//Non-blocking server
#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include <string.h>
#include <ctime>
#include <iomanip>
#include <string>
#include <fstream>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/timeb.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <poll.h>
#include <signal.h>
#include <vector>

typedef unsigned char BYTE;
typedef unsigned int DWORD;
typedef unsigned short WORD;

const int MSG_HEADER_LEN = 10;
const int FILE_HEADER_LEN = 10;

static std::time_t time_now = std::time(0);

#define MAX_REQUEST_SIZE 10000000
#define MAX_CONCURRENCY_LIMIT 288

struct CLIENTS
{
	struct sockaddr_in ip;
	int sockNo;
	std::string user = "";
	struct pollfd *socks[9];
	struct CONN_STAT *conns[9];
};

struct CONN_STAT
{
	long int nRecv;
	long int nSent;
	long int header;
	long int toRead;
	long int toSend;
	int pIndex;
	int cIndex;
	int nTrans = 0;
	int fflag;
	std::string fileName;
	std::vector<CONN_STAT *> fileSocks;
	std::string wBuf; //0 if unknown yet
	std::string rBuf;
};

int nConns;
int nClients = 0;									  //total # of data sockets
struct pollfd peers[MAX_CONCURRENCY_LIMIT + 1];		  //sockets to be monitored by poll()
struct CONN_STAT connStat[MAX_CONCURRENCY_LIMIT + 1]; //app-layer stats of the sockets
struct CLIENTS clients[32];

int addHeader(std::string &data, struct CONN_STAT *pStat)
{
	char msgLength[MSG_HEADER_LEN + 1];
	sprintf(msgLength, "%10d", data.size());
	std::string sendMsg = std::string(msgLength);
	sendMsg += data;
	pStat->wBuf.append(std::string(sendMsg));
	pStat->toSend += sendMsg.length();
	return 0;
}

void Error(struct CONN_STAT *pStat, const char *format, ...)
{
	char msg[4096];
	va_list argptr;
	va_start(argptr, format);
	vsprintf(msg, format, argptr);
	va_end(argptr);
	std::string err = "Error: " + std::string(msg) + "\n";
	addHeader(err, pStat);
}

void Log(const char *format, ...)
{
	char msg[2048];
	va_list argptr;
	va_start(argptr, format);
	vsprintf(msg, format, argptr);
	va_end(argptr);
	fprintf(stderr, "%s\n", msg);
}

void clear(struct CONN_STAT *pStat)
{
	memset((char *)&pStat->wBuf, 0, sizeof(pStat->wBuf));
	memset((char *)&pStat->nRecv, 0, sizeof(pStat->nRecv));
	memset((char *)&pStat->nSent, 0, sizeof(pStat->nSent));
	memset((char *)&pStat->toRead, 0, sizeof(pStat->toRead));
}

void RemoveConnection(int i)
{
	std::cout << inet_ntoa(clients[i].ip.sin_addr) << " disconnected" << std::endl;

	for (int h = 0; h < 9; h++)
	{
		linger lin;
		unsigned int y = sizeof(lin);
		lin.l_onoff = 1;
		lin.l_linger = 10;
		int r = setsockopt(clients[i].socks[h]->fd, SOL_SOCKET, SO_LINGER, (void *)(&lin), y);
		if (r != 0)
		{
			Log("Cannot enable SO_LINGER option.");
			exit(-1);
		}
		close(clients[i].socks[h]->fd);
		if (clients[i].conns[h]->pIndex < nConns)
		{
			memmove(peers + i, peers + i + 1, (nConns - i) * sizeof(struct pollfd));
			memmove(connStat + i, connStat + i + 1, (nConns - i) * sizeof(struct CONN_STAT));
		}
		nConns--;
	}
	//
	if (i < nClients)
	{
		memmove(clients + i, clients + i + 1, (nClients - i) * sizeof(struct CLIENTS));
	}

	nClients--;
}

int Send_NonBlocking(int sockFD, struct CONN_STAT *pStat, struct pollfd *pPeer)
{
	//have a BYTE* param? then need to check erase
	std::string sendMsg = pStat->wBuf;

	int sent = pStat->nSent;
	while (sent < pStat->toSend)
	{
		//pStat keeps tracks of how many bytes have been sent, allowing us to "resume"
		//when a previously non-writable socket becomes writable.
		int n = send(sockFD, sendMsg.c_str(), pStat->toSend - sent, 0);
		if (n >= 0)
		{
			pStat->nSent = sent += n;
			pStat->toSend -= sent;
			pStat->wBuf.erase(0, n);
		}
		else if (n < 0 && (errno == ECONNRESET || errno == EPIPE))
		{
			Log("Connection closed.");
			close(sockFD);
			return -1;
		}
		else if (n < 0 && (errno == EWOULDBLOCK))
		{
			//The socket becomes non-writable. Exit now to prevent blocking.
			//OS will notify us when we can write
			pPeer->events |= POLLWRNORM;
			return 1;
		}
		else
		{
			Error(pStat, "Unexpected send error %d: %s", errno, strerror(errno));
		}
	}
	pPeer->events &= ~POLLWRNORM;
	pStat->nSent = 0;
	return 0;
}

int getHeader(int sockFD, BYTE *data, struct CONN_STAT *pStat, struct pollfd *pPeer)
{
	int n; // The number of bytes recieved
	int len = MSG_HEADER_LEN;
	int rcvd = int(pStat->header);

	while (rcvd < len)
	{
		n = recv(sockFD, data + rcvd, MSG_HEADER_LEN - rcvd, 0);
		if (n > 0)
		{

			pStat->header = rcvd += n;
		}
		else if (n == 0 || (n < 0 && errno == ECONNRESET))
		{
			std::cout << clients[pStat->cIndex].user << " connection closed" << std::endl;
			RemoveConnection(pStat->cIndex);
			return -1;
		}
		else if (n < 0 && (errno == EWOULDBLOCK))
		{
			//The socket becomes non-readable. Exit now to prevent blocking.
			//OS will notify us when we can read
			return 1;
		}
		else
		{
			Log("Unexpected recv error %d: %s.", errno, strerror(errno));
			std::cout << clients[pStat->cIndex].user << " connection closed" << std::endl;
			RemoveConnection(pStat->cIndex);
			return -1;
		}
	}
	pStat->toRead = atoi((char *)data);
	memset((char *)&pStat->rBuf, 0, sizeof(pStat->rBuf));
	return 0;
}

int Recv_NonBlocking(int sockFD, BYTE *data, struct CONN_STAT *pStat, struct pollfd *pPeer)
{
	int len = pStat->toRead;
	int rcvd = int(pStat->nRecv);
	//char *buf = (char *)calloc(2048, sizeof(char));
	//pStat keeps tracks of how many bytes have been rcvd, allowing us to "resume"
	//when a previously non-readable socket becomes readable.
	while (rcvd < len)
	{

		int n = recv(sockFD, data + rcvd, len - rcvd, 0);
		if (n > 0)
		{
			pStat->nRecv = rcvd += n;
			
			//free(buf);
		}
		else if (n == 0 || (n < 0 && errno == ECONNRESET))
		{
			Log("%s connection closed.", clients[pStat->cIndex].user);
			close(sockFD);
			return -1;
		}
		else if (n < 0 && (errno == EWOULDBLOCK))
		{
			//The socket becomes non-readable. Exit now to prevent blocking.
			//OS will notify us when we can read
			return 1;
		}
		else
		{
			Error(pStat, "Unexpected recv error %d: %s.", errno, strerror(errno));
		}
	}

	return 0;
}

void SetNonBlockIO(int fd)
{
	int val = fcntl(fd, F_GETFL, 0);
	if (fcntl(fd, F_SETFL, val | O_NONBLOCK) != 0)
	{
		Log("Cannot set nonblocking I/O.");
		exit(-1);
	}
}

int parse_line(char *input, char *tokens[], char *delim)
{
	int i = 0;
	char *tok = strtok(input, delim);
	std::string cmd = std::string(tok);
	int n = cmd == "SEND" || cmd == "SENDA" || cmd == "SENDF" ? 2 : 3;

	while (tok != NULL && i < n)
	{
		tokens[i] = "";
		tokens[i] = tok;
		i++;
		if (i == (n - 1))
		{
			tok = strtok(NULL, "\r\n");
			tokens[i] = "";
			tokens[i] = tok;
			break;
		}
		else
			tok = strtok(NULL, delim);
	}
	i++;
	tokens[i] = NULL;
	return i;
}

void reset()
{
	std::ofstream ofs;
	ofs.open("db.txt", std::ofstream::out | std::ofstream::trunc);
	ofs.close();
}

int lookup(char *val, bool flag)
{
	unsigned int curLine = 0;
	//std::string line;
	char *line = NULL;
	size_t len = 0;
	FILE *db = fopen("db.txt", "a+");
	if (db != NULL)
	{
		while (getline(&line, &len, db) != -1)
		{
			if (std::string(line).find(val, 0) != std::string::npos)
			{
				return 0;
			}
		}
		if (flag)
		{
			fprintf(db, "%s\n", val);
			fclose(db);
			return 1;
		}
	}
	return -1;
}

int Register(int sockFD, char *usr, char *pwd, struct CONN_STAT *pStat, struct pollfd *pPeer, int index)
{
	std::string msg = std::string(usr) + "=" + std::string(pwd);
	int n = lookup((char *)msg.c_str(), true);
	if (n == 1)
	{
		std::string response = "Registered.\n";
		addHeader(response, pStat);
		Send_NonBlocking(sockFD, pStat, pPeer);
		std::cout << usr << " has registered" << std::endl;

		return 0;
	}
	if (n == 0)
	{
		std::string response = "Already registered\n";
		addHeader(response, pStat);
		Send_NonBlocking(sockFD, pStat, pPeer);
		std::cout << "REGISTRATION FAILED: " << usr << " is already registered" << std::endl;
	}
	return -1;
}

int login(int sockFD, char *usr, char *pwd, struct CONN_STAT *pStat, struct pollfd *pPeer, int index)
{
	std::string msg = std::string(usr) + "=" + std::string(pwd);

	if (lookup((char *)msg.c_str(), false) == 0)
	{
		std::string response = "Logged in.\n";
		addHeader(response, pStat);
		Send_NonBlocking(sockFD, pStat, pPeer);

		struct sockaddr_in user;
		socklen_t ulen = sizeof(user);
		getpeername(sockFD, (struct sockaddr *)&user, &ulen);
		for (int i = 0; i < nClients; i++)
		{
			if (clients[i].ip.sin_addr.s_addr == user.sin_addr.s_addr)
			{
				clients[i].user = usr;
				std::cout << clients[i].user << " has logged in" << std::endl;
				return 0;
			}
		}
		return 0;
	}
	std::string response = "Error: Incorrect username or password\n";
	addHeader(response, pStat);
	std::cout << "LOGIN FAILURE: "
			  << "<" << usr << "," << pwd << ">" << std::endl;
	Send_NonBlocking(sockFD, pStat, pPeer);
	RemoveConnection(pStat->cIndex);
	return -1;
}

int logout(int sockFD, struct CONN_STAT *pStat, struct pollfd *pPeer, int index)
{
	std::string response = "Logged out.\n";
	addHeader(response, pStat);
	Send_NonBlocking(sockFD, pStat, pPeer);
	std::cout << clients[pStat->cIndex].user << " has logged out" << std::endl;
	RemoveConnection(pStat->cIndex);
}

int list(int sockFD, struct CONN_STAT *pStat, struct pollfd *pPeer)
{
	std::string list = "Users online:\n";
	for (int i = 0; i < nClients; i++)
	{
		list += clients[i].user + "\n";
	}
	addHeader(list, pStat);
	Send_NonBlocking(sockFD, pStat, pPeer);
}

//flag is for whether sending file or not
int Send(int flag, char *data, int cIndex, std::string usr = "[PUBLIC ANON]")
{
	std::string msg = usr + ": " + std::string(data) + "\n";

	for (int i = 0; i < nClients; i++)
	{
		if (i != cIndex)
		{
			int fd = clients[i].socks[0]->fd;
			CONN_STAT *pStat = clients[i].conns[0];
			pollfd *pPeer = clients[i].socks[0];
			addHeader(msg, pStat);
			Send_NonBlocking(fd, pStat, pPeer);
		}
	}
}
//same thing
int Send2(int flag, char *data, std::string to, std::string from = "[PRIVATE ANON]")
{
	std::string msg = from + ": " + std::string(data) + "\n";
	;
	for (int i = 0; i <= nClients; i++)
	{
		if (clients[i].user == to)
		{
			int fd = clients[i].socks[0]->fd;
			CONN_STAT *pStat = clients[i].conns[0];
			pollfd *pPeer = clients[i].socks[0];
			addHeader(msg, pStat);
			Send_NonBlocking(fd, &connStat[i], &peers[i]);

			return 0;
		}
	}
	return -1;
}

//flag is for whether sending file or not
int SendF(std::string data, int cIndex, char *filename, struct CONN_STAT *pStat)
{
	for (int i = 0; i < nClients; i++)
	{
		if (i != cIndex)
		{
			for (int k = 1; k < 9; k++)
			{
				if (clients[i].conns[k]->wBuf.empty())
				{
					int fd = clients[i].socks[k]->fd;
					CONN_STAT *pStat = clients[i].conns[k];
					pollfd *pPeer = clients[i].socks[k];
					addHeader(data, pStat);
					Send_NonBlocking(fd, pStat, pPeer);
					pStat->fileSocks.push_back(pStat);
					goto NEXT_CLIENT;
				}
			}
		}
	NEXT_CLIENT:;
	}
}
//same thing
int SendF2(std::string data, std::string to, char *filename, struct CONN_STAT *pStat)
{
	for (int i = 0; i <= nClients; i++)
	{
		if (clients[i].user == to)
		{
			for (int k = 1; k < 9; k++)
			{
				if (clients[i].conns[k]->wBuf.empty())
				{
					int fd = clients[i].socks[k]->fd;
					CONN_STAT *pStat = clients[i].conns[k];
					pollfd *pPeer = clients[i].socks[k];
					addHeader(data, pStat);
					Send_NonBlocking(fd, pStat, pPeer);
					pStat->fileSocks.push_back(pStat);
					return 0;
				}
			}
		}
	}
	return -1;
}

void parse(int sockFD, char *data, struct CONN_STAT *pStat, struct pollfd *pPeer, int index)
{
	int numToks;
	char *tokens[4];
	char delimit[] = " \r\n";
	memset(&tokens[0], 0, sizeof(tokens));
	numToks = parse_line(data, tokens, delimit);

	std::string srcUsr = clients[pStat->cIndex].user;

	if (strcmp(tokens[0], "REGISTER") == 0)
	{
		Register(sockFD, tokens[1], tokens[2], pStat, pPeer, pStat->cIndex);
	}
	else if (strcmp(tokens[0], "LOGIN") == 0)
	{
		login(sockFD, tokens[1], tokens[2], pStat, pPeer, pStat->cIndex);
	}
	else if (strcmp(tokens[0], "LOGOUT") == 0)
	{
		logout(sockFD, pStat, pPeer, pStat->cIndex);
	}
	else if (strcmp(tokens[0], "SEND") == 0)
	{
		if (numToks != 2)
		{
			std::string msg = "Error: Incorrect number of public message arguments";
			addHeader(msg, pStat);
		}
		std::cout << std::ctime(&time_now) << srcUsr << ">[ALL]: " << tokens[1] << std::endl;
		Send(0, tokens[1], pStat->cIndex, srcUsr);
		std::string msg = "Public message sent.\n";
		addHeader(msg, pStat);
		Send_NonBlocking(pPeer->fd, pStat, pPeer);
	}
	else if (strcmp(tokens[0], "SEND2") == 0)
	{
		if (numToks != 3)
		{
			printf("%s\n", tokens);
			Error(pStat, "Incorrect number of private message arguments");
		}
		if (Send2(0, tokens[2], tokens[1], srcUsr) < 0)
		{
			std::string msg = "User does not exist or is not online\n";
			addHeader(msg, pStat);
			Send_NonBlocking(pPeer->fd, pStat, pPeer);
			std::cout << srcUsr << " > " << tokens[1] << " FAILED (Reciever Offline)" << std::endl;
		}
		else
		{
			std::cout << std::ctime(&time_now) << srcUsr << " >[" << tokens[1] << "]: " << tokens[2] << std::endl;
			std::string msg = "Private message sent.\n";
			addHeader(msg, pStat);
			Send_NonBlocking(pPeer->fd, pStat, pPeer);
		}
	}
	else if (strcmp(tokens[0], "SENDA") == 0)
	{
		if (numToks != 2)
		{
			Error(pStat, "Incorrect number of anon public message arguments");
		}
		std::cout << std::ctime(&time_now) << srcUsr << " (Anon)>[ALL]: " << tokens[1] << std::endl;
		Send(0, tokens[1], pStat->cIndex);
		std::string msg = "Public anonymous message sent.\n";
		addHeader(msg, pStat);
		Send_NonBlocking(pPeer->fd, pStat, pPeer);
	}
	else if (strcmp(tokens[0], "SENDA2") == 0)
	{
		if (numToks != 3)
		{
			Error(pStat, "Incorrect number of anon private message arguments");
		}
		if (Send2(0, tokens[2], tokens[1]) < 0)
		{
			std::string msg = std::string(tokens[2]) + " does not exist or is not online\n";
			addHeader(msg, pStat);
			Send_NonBlocking(sockFD, pStat, pPeer);
		}
		else
		{
			std::cout << std::ctime(&time_now) << srcUsr << " (Anon)>[" << tokens[1] << "]: " << tokens[2] << std::endl;
			std::string msg = "Private anonymous message sent.\n";
			addHeader(msg, pStat);
			Send_NonBlocking(pPeer->fd, pStat, pPeer);
		}
	}
	else if (strcmp(tokens[0], "SENDF") == 0)
	{
		if (numToks != 2)
		{
			Error(pStat, "Incorrect number of public file transfer arguments");
		}
		std::cout << std::ctime(&time_now) << srcUsr << ">[ALL]: "
				  << "downloading " << tokens[1] << std::endl;
		pStat->fileName.append(std::string(tokens[1]));
		std::string temp = std::string(tokens[0]) + " " + std::string(tokens[1]);
		SendF(temp, pStat->cIndex, tokens[1], pStat);
		std::string msg = "Public file sent.\n";
		addHeader(msg, clients[pStat->cIndex].conns[0]);
		Send_NonBlocking(pPeer->fd, pStat, pPeer);
	}
	else if (strcmp(tokens[0], "SENDF2") == 0)
	{
		if (numToks != 3)
		{
			Error(pStat, "Incorrect number of anon private file transfer arguments");
		}
		std::cout << std::ctime(&time_now) << srcUsr << ">[" << tokens[1] << "]: "
				  << "file " << tokens[2] << std::endl;
		pStat->fileName = std::string(tokens[2]);
		std::string temp = std::string(tokens[0]) + " " + srcUsr + " " + std::string(tokens[2]);
		SendF2(temp, tokens[1], tokens[2], pStat);
		std::string msg = "Private file sent.\n";
		addHeader(msg, clients[pStat->cIndex].conns[0]);
		Send_NonBlocking(pPeer->fd, pStat, pPeer);
	}
	else if (strcmp(tokens[0], "LIST") == 0)
	{
		list(sockFD, pStat, pPeer);
		std::cout << std::ctime(&time_now) << srcUsr << " requested list of users " << std::endl;
	}
}

void DoServer(int svrPort, int maxConcurrency)
{
	int listenFD = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenFD < 0)
	{
		Log("Cannot create listening socket.");
		exit(-1);
	}
	SetNonBlockIO(listenFD);

	struct sockaddr_in serverAddr;
	memset(&serverAddr, 0, sizeof(struct sockaddr_in));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons((unsigned short)svrPort);
	serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);

	int optval = 1;
	int r = setsockopt(listenFD, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	if (r != 0)
	{
		Log("Cannot enable SO_REUSEADDR option.");
		exit(-1);
	}
	signal(SIGPIPE, SIG_IGN);

	if (bind(listenFD, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) != 0)
	{
		Log("Cannot bind to port %d.", svrPort);
		exit(-1);
	}

	if (listen(listenFD, 16) != 0)
	{
		Log("Cannot listen to port %d.", svrPort);
		exit(-1);
	}

	nConns = 0;
	memset(peers, 0, sizeof(peers));
	peers[0].fd = listenFD;
	peers[0].events = POLLRDNORM;
	memset(connStat, 0, sizeof(connStat));
	//set more mem here

	int connID = 0;
	while (1)
	{ //the main loop
		//monitor the listening sock and data socks, nConn+1 in total
		int nReady = poll(peers, nConns + 1, -1);
		if (nReady < 0)
		{
			Log("Invalid poll() return value.");
			exit(-1);
		}

		struct sockaddr_in clientAddr;
		socklen_t clientAddrLen = sizeof(clientAddr);

		//new incoming connection
		if ((peers[0].revents & POLLRDNORM) && (nConns < maxConcurrency))
		{
			int fd = accept(listenFD, (struct sockaddr *)&clientAddr, &clientAddrLen);
			if (fd != -1)
			{
				SetNonBlockIO(fd);
				nConns++;
				peers[nConns].fd = fd;
				peers[nConns].events = POLLRDNORM;
				peers[nConns].revents = 0;

				memset(&connStat[nConns], 0, sizeof(struct CONN_STAT));
				for (int j = 0; j < nClients; j++)
				{
					if (clients[j].ip.sin_addr.s_addr == clientAddr.sin_addr.s_addr)
					{
						clients[j].socks[clients[j].sockNo] = &peers[nConns];
						clients[j].conns[clients[j].sockNo++] = &connStat[nConns];
						connStat[nConns].cIndex = j;
						connStat[nConns].pIndex = nConns;
						goto BREAK_OUT;
					}
				}
				memset(&clients[nClients], 0, sizeof(struct CLIENTS));
				memset(clients[nClients].socks, 0, sizeof(clients[nClients].socks));
				memset(clients[nClients].conns, 0, sizeof(clients[nClients].conns));
				clients[nClients].socks[clients[nClients].sockNo] = &peers[nConns];
				clients[nClients].conns[clients[nClients].sockNo++] = &connStat[nConns];
				connStat[nConns].cIndex = nClients;
				connStat[nConns].pIndex = nConns;
				clients[nClients++].ip = clientAddr;
			}

			if (--nReady <= 0)
				continue;
		}
	BREAK_OUT:;

		for (int i = 1; i <= nConns; i++)
		{
			if (peers[i].revents & (POLLRDNORM | POLLERR | POLLHUP))
			{
				int fd = peers[i].fd;

				if (connStat[i].header < 10)
				{
					getHeader(fd, (BYTE *)&connStat[i].rBuf, &connStat[i], &peers[i]);
				}

				if (!(connStat[i].toRead == 0) && connStat[i].nRecv < connStat[i].toRead)
				{
					if (Recv_NonBlocking(fd, (BYTE *)&connStat[i].rBuf, &connStat[i], &peers[i]) < 0)
					{
						Error(&connStat[i], "Recieve from client failed");
						RemoveConnection(connStat[i].cIndex);
						goto NEXT_CONNECTION;
					}
				}

				if (connStat[i].header == 10 && connStat[i].nRecv == connStat[i].toRead && connStat[i].fileName.empty())
				{
					parse(fd, (char *)&connStat[i].rBuf, &connStat[i], &peers[i], i);
					connStat[i].toRead = 0;
					connStat[i].header = 0;
					connStat[i].nRecv = 0;
					memset(&connStat[i].rBuf, 0, sizeof(connStat[i].rBuf));
				}
				if (!connStat[i].fileName.empty())
				{

					getHeader(fd, (BYTE *)&connStat[i].rBuf, &connStat[i], &peers[i]);
					if (!(connStat[i].toRead == 0) && connStat[i].nRecv < connStat[i].toRead)
					{
						if (Recv_NonBlocking(fd, (BYTE *)&connStat[i].rBuf, &connStat[i], &peers[i]) < 0)
						{
							Error(&connStat[i], "Recieve from client failed");
							RemoveConnection(connStat[i].cIndex);
							goto NEXT_CONNECTION;
						}
					}
					if (!(connStat[i].toRead == 0) && connStat[i].nRecv == connStat[i].toRead)
					{
						while (!connStat[i].fileSocks.empty())
						{
							CONN_STAT *temp = connStat[i].fileSocks.back();
							addHeader(connStat[i].rBuf, temp);
							Send_NonBlocking(peers[temp->pIndex].fd, temp, &peers[temp->pIndex]);
							connStat[i].fileSocks.pop_back();
						}
						connStat[i].toRead = 0;
						connStat[i].header = 0;
						connStat[i].nRecv = 0;
						memset(&connStat[i].rBuf, 0, sizeof(connStat[i].rBuf));
						memset(&connStat[i].fileName, 0, sizeof(connStat[i].fileName));
					}
				}
			}

			//a previously blocked data socket becomes writable
			if (peers[i].revents & POLLWRNORM)
			{
				if (connStat[i].nSent < connStat[i].toSend)
				{
					int fd = peers[i].fd;
					if (Send_NonBlocking(peers[i].fd, &connStat[i], &peers[i]) < 0)
					{
						RemoveConnection(connStat[i].cIndex);
						goto NEXT_CONNECTION;
					}
				}
			}

		NEXT_CONNECTION:;
		}
	}
}

int main(int argc, char **argv)
{
	if (argc != 2)
	{
		Log("Usage: %s [server Port]", argv[0]);
		return -1;
	}

	if (strcmp(argv[1], "reset") == 0)
	{
		reset();
		exit(0);
	}
	std::cout << "Starting server\n";
	int port = atoi(argv[1]);
	int maxConcurrency = MAX_CONCURRENCY_LIMIT;
	DoServer(port, maxConcurrency);
	printf("ended\n");
	return 0;
}
