//Client
#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include <fstream>
#include <string.h>
#include <sys/types.h>
#include <string>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/timeb.h>
#include <fcntl.h>
#include <stdarg.h>
#include <poll.h>
#include <signal.h>

typedef unsigned char BYTE;
typedef unsigned int DWORD;
typedef unsigned short WORD;

#define MAX_FILE_SIZE 10000000

const int MSG_HEADER_LEN = 10;
const int FILE_HEADER_LEN = 10;
int d = -1;

struct sockaddr_in serverAddr;
const char *serverIP;
int port;

FILE *fp;

struct CONN_STAT
{
	int nRecv;
	int nSent;
	std::string user = "";
	long int header;
	long int fRead;
	long int toRead;
	long int toSend;
	struct stat file_stat;
	int fd;
	off_t offset;
	char *fName;
	int fSize;
	std::string rBuf;
	std::string wBuf;
};

int nConns;					  //total # of data sockets
struct pollfd peers[9];		  //sockets to be monitored by poll()
struct CONN_STAT connStat[9]; //app-layer stats of the sockets

bool login = false;
std::string username = "";

void closeSocks()
{
	for (int i = 0; i < 9; i++)
	{
		close(peers[i].fd);
	}
}

void clearFile(char *file)
{
	std::ofstream ofs;
	ofs.open(file, std::ofstream::out | std::ofstream::trunc);
	ofs.close();
}

void Error(const char *format, ...)
{
	char msg[4096];
	va_list argptr;
	va_start(argptr, format);
	vsprintf(msg, format, argptr);
	va_end(argptr);
	fprintf(stderr, "Error: %s\n", msg);
	fclose(fp);
	closeSocks();
	exit(-1);
}

void Log(const char *format, ...)
{
	char msg[2048];
	va_list argptr;
	va_start(argptr, format);
	vsprintf(msg, format, argptr);
	va_end(argptr);
	fprintf(stderr, "%s", msg);
}

void clear(struct CONN_STAT *pStat)
{
	memset((char *)&pStat->rBuf, 0, sizeof(pStat->rBuf));
	memset((char *)&pStat->nRecv, 0, sizeof(pStat->nRecv));
	memset((char *)&pStat->nSent, 0, sizeof(pStat->nSent));
	memset((char *)&pStat->toRead, 0, sizeof(pStat->toRead));
	memset((char *)&pStat->toSend, 0, sizeof(pStat->toSend));
	memset(&pStat->file_stat, 0, sizeof(pStat->file_stat));
}

void SetNonBlockIO(int fd)
{
	int val = fcntl(fd, F_GETFL, 0);
	if (fcntl(fd, F_SETFL, val | O_NONBLOCK) != 0)
	{
		Error("Cannot set nonblocking I/O.");
	}
}

void intHandler(int dummy)
{
	fclose(fp);
	closeSocks();
	Log("Connection closed.");
	exit(1);
}

void CheckData(char *data, int size, char *msg, int flag = 0)
{
	for (int i = 0; i < size; i++)
	{
		if (flag == 0)
		{
			if (!((data[i] >= 'a' && data[i] <= 'z') || (data[i] >= 'A' && data[i] <= 'Z') || (data[i] >= '0' && data[i] <= '9')))
			{
				Error("%s", msg);
			}
		}
		else
		{
			if (data[i] == '/')
			{
				Error("%s", msg);
			}
		}
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
		tokens[i] = tok;
		i++;
		if (i == (n - 1))
		{
			tok = strtok(NULL, "\r\n");
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

int Send_NonBlocking(int sockFD, struct CONN_STAT *pStat, struct pollfd *pPeer)
{
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
			Error("Unexpected send error %d: %s", errno, strerror(errno));
		}
	}
	pPeer->events &= ~POLLWRNORM;
	pStat->nSent = 0;
	return 0;
}

int sFile(int sockFD, struct CONN_STAT *pStat, struct pollfd *pPeer)
{
	std::string sendMsg = pStat->wBuf;

	ssize_t sent = pStat->nSent;
	while (sent < pStat->file_stat.st_size)
	{
		//pStat keeps tracks of how many bytes have been sent, allowing us to "resume"
		//when a previously non-writable socket becomes writable.
		int n = sendfile(sockFD, pStat->fd, &pStat->offset, pStat->file_stat.st_size - sent);
		if (n >= 0)
		{
			pStat->nSent = sent += n;
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
			Error("Unexpected send error %d: %s", errno, strerror(errno));
		}
	}
	pPeer->events &= ~POLLWRNORM;
	pStat->offset = 0;
	memset(&pStat->file_stat, 0, sizeof(pStat->file_stat));
	pStat->fd = 0;
	pStat->nSent = 0;
	return 0;
}

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

int sendH(std::string &cmd, char *file, int sockFD, struct CONN_STAT *pStat, struct pollfd *pPeer)
{

	CheckData(file, std::string(file).size(), "File must be in same folder", 1);
	int fd;
	struct stat file_stat;
	off_t offset;
	int sent_bytes;
	int remain_data;

	char msgLength[MSG_HEADER_LEN + 1];

	pStat->fd = open(file, O_RDONLY);
	if (fd == -1)
	{
		Error("Cannot open file");
	}
	if (stat(file, &file_stat) < 0)
	{
		Error("fstat error");
	}
	if (file_stat.st_size > MAX_FILE_SIZE)
	{
		Error("File to send exceeds 10MB");
	}

	sprintf(msgLength, "%10d", file_stat.st_size);
	pStat->file_stat = file_stat;

	pStat->wBuf.append(std::string(msgLength));
	pStat->toSend += sizeof(msgLength);
	return 0;
}

int addToFBuf(std::string &data, char *file)
{
	for (int i = 1; i < 9; i++)
	{
		if (connStat[i].wBuf.empty())
		{
			addHeader(data, &connStat[i]);
			sendH(data, file, peers[i].fd, &connStat[i], &peers[i]);
			Send_NonBlocking(peers[i].fd, &connStat[i], &peers[i]);
			sFile(peers[i].fd, &connStat[i], &peers[i]);
			return 0;
		}
	}
	return -1;
}

int download(int sockFD, char *file, struct CONN_STAT *pStat, struct pollfd *pPeer)
{
	int sent = pStat->nSent;
	while (sent < pStat->toSend)
	{
		//pStat keeps tracks of how many bytes have been sent, allowing us to "resume"
		//when a previously non-writable socket becomes writable.
		int n = write(sockFD, &pStat->rBuf + sent, pStat->toSend - sent);
		if (n >= 0)
		{
			pStat->nSent = sent += n;
			pStat->toSend -= sent;
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
			Error("Unexpected send error %d: %s", errno, strerror(errno));
		}
	}
	pPeer->events &= ~POLLWRNORM;
	pStat->nSent = 0;
	close(sockFD);
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
			std::cout << pStat->user << " connection closed" << std::endl;
			close(sockFD);
			Error("Error retrieving request header.");
		}
		else if (n < 0 && (errno == EWOULDBLOCK))
		{
			//The socket becomes non-readable. Exit now to prevent blocking.
			//OS will notify us when we can read
			return 1;
		}
		else
		{
			Error("Unexpected recv error %d: %s.", errno, strerror(errno));
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
	char *buf = (char *)calloc(2048, sizeof(char));
	//pStat keeps tracks of how many bytes have been rcvd, allowing us to "resume"
	//when a previously non-readable socket becomes readable.
	while (rcvd < len)
	{

		int n = recv(sockFD, buf, len - rcvd, 0);
		if (n > 0)
		{
			pStat->nRecv = rcvd += n;
			pStat->rBuf.append(std::string(buf));
			free(buf);
		}
		else if (n == 0 || (n < 0 && errno == ECONNRESET))
		{
			std::cout << pStat->user << " connection closed" << std::endl;
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
			Error("Unexpected recv error %d: %s.", errno, strerror(errno));
		}
	}

	return 0;
}

void validate(char *user, char *pwd)
{
	std::string valid = std::string(user) + std::string(pwd);
	int usr = std::string(user).size();
	int ps = std::string(pwd).size();
	if ((usr > 8 || usr < 4) || (ps > 8 || ps < 4))
	{
		Error("Incorrect length for username or password");
	}
	CheckData((char *)valid.c_str(), valid.size(), "Incorrect values in username or password");
}

void Register(char *user, char *pwd)
{
	validate(user, pwd);
	std::string msg = "REGISTER " + std::string(user) + " " + std::string(pwd);
	addHeader(msg, &connStat[0]);
	return;
}

int Login(char *user, char *pwd)
{
	if (login)
	{
		printf("Already logged in\n");
		return 0;
	}

	validate(user, pwd);
	std::string msg = "LOGIN " + std::string(user) + " " + std::string(pwd);
	addHeader(msg, &connStat[0]);
	username = std::string(user);
	return 0;
}

void Logout()
{
	std::string msg = "LOGOUT";
	addHeader(msg, &connStat[0]);
	login = false;

	// close(sockFD);
	// fclose(fp);
	// Log("Logged Out");
}

void List()
{
	std::string msg = "LIST";
	addHeader(msg, &connStat[0]);

	return;
}

int Send(char *cmd, char *data)
{
	std::string msg = std::string(cmd) + " " + std::string(data);
	addHeader(msg, &connStat[0]);
	return 0;
}

int Send2(char *cmd, std::string to, char *data)
{
	std::string msg = std::string(cmd) + " " + to + " " + std::string(data);
	addHeader(msg, &connStat[0]);
	return 0;
}

void delay(int n)
{
	//add a recv timeout
	//std::cout << "Delaying " << n << "s..." << std::endl;
	d = n * 1000;
	// sleep(n);
}

int parse(char *line, int sockFD, struct CONN_STAT *pStat, struct pollfd *pPeer)
{
	int numToks;
	char *tokens[4];
	char delimit[] = " \r\n";

	numToks = parse_line(line, tokens, delimit);

	if (strcmp(tokens[0], "REGISTER") == 0)
	{
		if (numToks != 3)
		{
			Error("Incorrect number of register arguments");
		}
		Register(tokens[1], tokens[2]);
		Send_NonBlocking(sockFD, pStat, pPeer);
		return 0;
	}
	else if (strcmp(tokens[0], "LOGIN") == 0)
	{
		if (numToks != 3)
		{
			Error("Incorrect number of login arguments");
		}
		Login(tokens[1], tokens[2]);
		login = true;
		Send_NonBlocking(sockFD, pStat, pPeer);
		return 0;
	}
	else if (strcmp(tokens[0], "LOGOUT") == 0)
	{
		Logout();
		return 0;
	}
	else if (strcmp(tokens[0], "SEND") == 0)
	{
		if (!login)
			Error("Must log in first");
		if (numToks != 2)
		{
			Error("Incorrect number of public message arguments");
		}
		Send(tokens[0], tokens[1]);
		Send_NonBlocking(sockFD, pStat, pPeer);
		return 0;
	}
	else if (strcmp(tokens[0], "SEND2") == 0)
	{
		if (!login)
			Error("Must log in first");
		else if (strcmp(tokens[1], username.c_str()) == 0)
			Error("Can't send message to self");
		if (numToks != 3)
		{
			Error("Incorrect number of private message arguments");
		}
		Send2(tokens[0], tokens[1], tokens[2]);
		Send_NonBlocking(sockFD, pStat, pPeer);
		return 0;
	}
	else if (strcmp(tokens[0], "SENDA") == 0)
	{
		if (!login)
			Error("Must log in first");
		if (numToks != 2)
		{
			Error("Incorrect number of anon public message arguments");
		}
		Send(tokens[0], tokens[1]);
		Send_NonBlocking(sockFD, pStat, pPeer);
		return 0;
	}
	else if (strcmp(tokens[0], "SENDA2") == 0)
	{
		if (!login)
			Error("Must log in first");
		if (numToks != 3)
		{
			Error("Incorrect number of anon private message arguments");
		}
		Send2(tokens[0], tokens[1], tokens[2]);
		Send_NonBlocking(sockFD, pStat, pPeer);
		return 0;
	}
	else if (strcmp(tokens[0], "SENDF") == 0)
	{
		if (!login)
			Error("Must log in first");
		if (numToks != 2)
		{
			Error("Incorrect number of public file transfer arguments");
		}
		std::string temp = std::string(tokens[0]) + " " + std::string(tokens[1]);
		addToFBuf(temp, tokens[1]);
		return -1;
	}
	else if (strcmp(tokens[0], "SENDF2") == 0)
	{
		if (!login)
			Error("Must log in first");
		else if (strcmp(tokens[1], username.c_str()) == 0)
			Error("Can't send file to self");
		if (numToks != 3)
		{
			Error("Incorrect number of anon private file transfer arguments");
		}
		std::string temp = std::string(tokens[0]) + " " + std::string(tokens[1]) + " " + std::string(tokens[2]);
		addToFBuf(temp, tokens[1]);
		return -2;
	}
	else if (strcmp(tokens[0], "LIST") == 0)
	{
		List();
		Send_NonBlocking(sockFD, pStat, pPeer);
		return 0;
	}
	else if (strcmp(tokens[0], "DELAY") == 0)
	{
		if (atoi(tokens[1]) == 9999)
		{ d = 0;
			return 0;}
		delay(atoi(tokens[1]));
		// pStat->header = pStat->toRead = pStat->nRecv = 10;
		return 0;
	}
	else
	{
		Error("Unknown or incorrect command detected");
	}
}

int main(int argc, char **argv)
{

	if (argc != 4)
	{
		Log("Usage: %s [server IP] [server Port] [script filepath]", argv[0]);
		return -1;
	}

	serverIP = argv[1];
	port = atoi(argv[2]);
	const char *script = argv[3];
	fp = fopen(script, "r");
	if (fp == NULL)
	{
		Error("Cannot read test file");
	}
	std::cout << "Starting client\n";
	memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons((unsigned short)port);
	inet_pton(AF_INET, serverIP, &serverAddr.sin_addr);

	//ignore the SIGPIPE signal that may crash the program in some corner cases
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, intHandler);

	nConns = 0;
	memset(peers, 0, sizeof(peers));
	memset(connStat, 0, sizeof(connStat));

	for (int i = 0; i < 9; i++)
	{
		//Create the socket
		int sockFD = socket(AF_INET, SOCK_STREAM, 0);
		if (sockFD == -1)
		{
			Error("Cannot create socket.");
		}
		linger lin;
		unsigned int y = sizeof(lin);
		lin.l_onoff = 1;
		lin.l_linger = 10;
		int r = setsockopt(sockFD, SOL_SOCKET, SO_LINGER, (void *)(&lin), y);
		if (r != 0)
		{
			Log("Cannot enable SO_LINGER option.");
			exit(-1);
		}
		//Connect to server
		if (connect(sockFD, (const struct sockaddr *)&serverAddr, sizeof(serverAddr)) != 0)
		{
			Error("Cannot connect to server %s:%d.", serverIP, port);
		}

		peers[i].fd = sockFD;
		peers[i].events = POLLRDNORM;
		peers[i].revents = 0;

		memset(&connStat[i], 0, sizeof(struct CONN_STAT));
	}

	int s = -1;

	while (1)
	{
		char *line = NULL;
		size_t len = 0;

		if (getline(&line, &len, fp) != -1)
		{
			parse(line, peers[0].fd, &connStat[0], &peers[0]);
		}

		int nReady = poll(peers, 9, d);
		if (nReady < 0)
		{
			Error("Invalid poll() return value.");
		}

		if (peers[0].revents & POLLWRNORM)
		{
			int fd = peers[0].fd;
			if (connStat[0].toSend != 0)
			{
				if (connStat[0].nSent < connStat[0].toSend)
				{
					if (Send_NonBlocking(peers[0].fd, &connStat[0], &peers[0]) < 0)
					{
						Error("Send to server failed");
					}
				}
				if (connStat[0].nSent == connStat[0].toSend)
				{
					connStat[0].toSend = 0;
					connStat[0].nSent = 0;
				}
			}
		}

		if (strcmp(line, "DELAY") != 0 && peers[0].revents & (POLLRDNORM | POLLERR | POLLHUP))
		{
			int fd = peers[0].fd;
			if (connStat[0].header < 10)
			{
				getHeader(fd, (BYTE *)&connStat[0].rBuf, &connStat[0], &peers[0]);
			}

			if (!(connStat[0].toRead == 0) && connStat[0].nRecv < connStat[0].toRead)
			{
				if (Recv_NonBlocking(fd, (BYTE *)&connStat[0].rBuf, &connStat[0], &peers[0]) < 0)
				{
					Error("Recieve from server failed");
				}
			}

			if (!(connStat[0].toRead == 0) && connStat[0].nRecv == connStat[0].toRead)
			{
				//parse response here
				std::cout << connStat[0].rBuf;
				char *tok = strtok((char *)connStat[0].rBuf.c_str(), " \r\n");
				if (strcmp(tok, "Error:") == 0)
				{
					closeSocks();
					Log("Connection closed.");
					exit(1);
				}
				connStat[0].toRead = 0;
				connStat[0].nRecv = 0;
				connStat[0].header = 0;
				memset(&connStat[0].rBuf, 0, sizeof(connStat[0].rBuf));
			}
		}

		for (int i = 1; i < 9; i++)
		{
			if (peers[i].revents & (POLLRDNORM | POLLERR | POLLHUP))
			{
				int fd = peers[i].fd;
				if (sizeof(connStat[i].header) < 10)
				{
					getHeader(fd, (BYTE *)&connStat[i].toRead, &connStat[i], &peers[i]);
				}

				if ((connStat[i].header == 10) && connStat[i].nRecv < connStat[i].toRead)
				{
					if (Recv_NonBlocking(fd, (BYTE *)&connStat[i].rBuf, &connStat[i], &peers[i]) < 0)
					{
						Error("Recieve from server failed");
					}
				}

				if ( connStat[i].fSize != 0 && !(connStat[i].toRead == 0) && connStat[i].nRecv == connStat[i].toRead)
				{
					//parse response here
					char *cmd = strtok((char *)connStat[i].rBuf.c_str(), " \r\n");
					char *from = strtok((char *)connStat[i].rBuf.c_str(), " \r\n");
					char *file = strtok((char *)connStat[i].rBuf.c_str(), " \r\n");
					if (strcmp(cmd, "SENDF") == 0){
						std::cout << "Private file " + std::string(file) + " sent from " + from << std::endl;
					}
					else
					{
						std::cout << "Public file " + std::string(file) + " sent from " + from << std::endl;
					}
					int fileD = open(file, O_WRONLY | O_CREAT);
					if (sizeof(connStat[i].fName) == 0){
						connStat[i].fName = file;
						//clear here
					}
					download(fileD, file, &connStat[i], &peers[i]);
					connStat[i].toRead = 0;
					connStat[i].nSent = 0;
					memset(&connStat[i].rBuf, 0, sizeof(connStat[i].rBuf));
				}
			}

			//a previously blocked data socket becomes writable
			if (peers[i].revents & POLLWRNORM)
			{
				int fd = peers[i].fd;
				if (connStat[i].header < 10)
				{
					if (Send_NonBlocking(peers[i].fd, &connStat[i], &peers[i]) < 0)
					{
						Error("Send to server failed");
					}
				}
				if (!(connStat[i].toSend == 0) && connStat[i].nSent < connStat[i].toSend)
				{
					if (Send_NonBlocking(peers[i].fd, &connStat[i], &peers[i]) < 0)
					{
						Error("Send to server failed");
					}
				}
				if (!(connStat[i].toSend == 0) && connStat[i].nSent == connStat[i].toSend)
				{
					connStat[i].toSend = 0;
					connStat[i].nSent = 0;
				}
			}

		NEXT_CONNECTION:;
		}
	}

	return 0;
}
