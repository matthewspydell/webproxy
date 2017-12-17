#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <signal.h>
#include <openssl/md5.h>
#include <time.h>

void startProxy(char* PORT);
int clientHandler(int client_sock);
void forwardData(int dest_sock, int src_sock, char* host, char* path, int remote_flag);
int connectRemote(char* HOST, int client_sock);
int IPcached(char* HOST);
int cached_and_timeout(char* path, char* host, int tout);
void fntohash(char* hash, char* filename);
int on_blacklist(char* HOST);

#define MAXBUFSIZE 1000
#define MAXCONNQUEUE 100
#define printError(str) printf("%s: (%d) %s\n", #str, errno, strerror(errno));

int client_sock, proxy_sock, remote_sock, timeout = 60;

void sigint_handler(int signal) {
	printf("\nClosing sockets and exiting gracefully... goodbye\n");
	close(client_sock);
	close(proxy_sock);
	close(remote_sock);
	exit(0);
}

/******************** Main Loop ********************/
int main (int argc, char * argv[] )	{

	if (argc == 3) timeout = atoi(argv[2]);

	struct sockaddr_in client_addr;
	socklen_t addrlen = sizeof(client_addr);

	signal(SIGINT, sigint_handler);		// handle ctrl+C

	startProxy(argv[1]);				// start proxy with given port number

	while (1) {

		// block until first connection
		if ((client_sock = accept(proxy_sock, (struct sockaddr *)&client_addr, &addrlen)) < 0) {
			printf("Error accepting connection\n");
			exit(-1);
		} else {
			if (fork() == 0) {
				close(proxy_sock);
				clientHandler(client_sock);
				return 0;
			}
			close(client_sock);
		}
	}
}

void startProxy(char *PORT) {

	// setup proxy to listen for incoming connections
	struct sockaddr_in proxy;     //Internet socket address structure

	bzero(&proxy,sizeof(proxy));                  //zero the struct
	proxy.sin_family = AF_INET;                   //address family IPV4
	proxy.sin_port = htons(atoi(PORT));        	  //htons() sets the port # to network byte order
	proxy.sin_addr.s_addr = INADDR_ANY;           //supplies the IP address of the local machine

	// Create socket with TCP
	if ((proxy_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		printf("Unable to create socket");
		exit(-1);
	}

	if (bind(proxy_sock, (struct sockaddr *)&proxy, sizeof(proxy)) < 0) {
		printf("Unable to bind socket\n");
		exit(-1);
	}

	if (listen(proxy_sock, MAXCONNQUEUE) < 0) { // Start listening for connections on port
		printf("Error listening\n");
		exit(-1);
	}
	printf("Proxy listening on port %s\n", PORT);
}


/* This function handles a client's request to contact a server */
int clientHandler(int client_sock) {
	char* request[4], buffer[MAXBUFSIZE];

	char* badRequest = "HTTP/1.0 400 Bad Request\n\n";
	char badMethod[100] = "<html><body>400 Bad Request Reason: Invalid Method: <";
	char* closeHTML = "></body></html>";

	// Peek at header information from client
	if (recv(client_sock, buffer, MAXBUFSIZE, MSG_PEEK) < 0) {
		printError(Error receiving client request);
		shutdown(client_sock, SHUT_RDWR); // shutdown socket for further reading and writing
		close(client_sock); // close connection
		exit(-1);
	}
	printf("\nHTTP request:%s\n", buffer);

	// Parse request from client
	request[0] = strtok(buffer, " \r\n");	// method
	request[1] = strtok(NULL, " \r\n");	// file
	request[2] = strtok(NULL, " \r\n");	// HTTP version
	strtok(NULL, " \r\n");
	request[3] = strtok(NULL, " \r\n");	// host

	printf("request0: %s\n", request[0]);
	printf("request1: %s\n", request[1]);
	printf("request2: %s\n", request[2]);
	printf("request3: %s\n", request[3]);

	if (strcmp(request[0], "GET") != 0) { // received bad method (respond accordingly)
			printf("Request method not supported\n");
			write(client_sock, badRequest, strlen(badRequest)); // invalid method
			strcat(badMethod, request[0]);
			strcat(badMethod, closeHTML);
			write(client_sock, badMethod, strlen(badMethod));
	} else {
		if (!on_blacklist(request[3])) {
			// if file isn't cached or timeout is met then get data from remote server
			if (!cached_and_timeout(request[3], request[1], timeout)) {
				printf("Rerouting request to %s\n", request[3]);
				printf("Connecting to %s...\n", request[3]);

				if ((remote_sock = connectRemote(request[3], client_sock)) < 0) {printf("failed\n");}
				else {
					printf("success\n");

					printf("Entering client to remote forwarding\n");
					forwardData(remote_sock, client_sock, request[3], request[1], 0);

					printf("Entering remote to client forwarding\n");
					forwardData(client_sock, remote_sock, request[3], request[1], 1);

					printf("Shutting down client and remote connections\n");
					shutdown(client_sock, SHUT_RDWR);
					shutdown(remote_sock, SHUT_RDWR);
				}
				close(remote_sock);
			} else {	// else data is cached so pull data from cache instead of remote server
				printf("Found data in cache\n");
				printf("Sending data from cache...\n");

				int fd, nbytes;
				char filename[200];
				char hash[MD5_DIGEST_LENGTH * 2 + 1];

				// construct filename based on requested file (path), from host
				strcat(filename, request[3]);
				strcat(filename, "_");
				strcat(filename, request[1]);

				// create hash of filename to use to create file in cache directory
				fntohash(hash, filename);

				// file is located in cache directory (./cache/<hash>)
				memset(filename, 0, sizeof(filename));
				strcpy(filename, "./cache/");
				strcat(filename, hash);

				if ((fd = open(filename, O_RDONLY)) < 0) {
					printError(Could not open cached file);
				}

				// send data to client
				while ((nbytes = read(fd, buffer, MAXBUFSIZE)) > 0) {
					write (client_sock, buffer, nbytes);
					return -1;
				}
				printf("Finished\n");
			}
		} else { // website is blacklisted return 403 Forbidden
			printf("Host is forbidden\n");
			char forb[] = "HTTP/1.0 403 Forbidden\n\n<html><body>403 Forbidden </body></html>";
			write(client_sock, forb, strlen(forb));
		}
	}
	close(client_sock);
	return 0;
}


/* This function creates a connection from the proxy to the remote host specified by the client request */
int connectRemote(char* HOST, int client_sock) {
	struct addrinfo hints, *remote_addr;
	int sock, rv, fd;
	char cacheDir[200] = "./cache/";

	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		printError(Could not create socket);
		return -1;
	}

	if (!IPcached(HOST)) { // if IP is not cached then do a DNS lookup

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		if ((rv = getaddrinfo(HOST, "http", &hints, &remote_addr)) < 0) {
			if (rv == EAI_SYSTEM) { printError(Could not locate remote host); }
			else { printf("Error getting remote address information: EAI(%d) %s\n", rv, gai_strerror(rv)); }

			char nfound[] = "HTTP/1.0 404 Not Found\n\n<html><body>404 Server Not Found </body></html>";
			write(client_sock, nfound, strlen(nfound));

			return -1;
		}

		//printf("PORT: %d\n", ntohs(((struct sockaddr_in*)remote_addr->ai_addr)->sin_port));

		// convert IP to string and save to file in cache
		char* IP = inet_ntoa(((struct sockaddr_in*)remote_addr->ai_addr)->sin_addr);
		printf("%s -> %s\n", HOST, IP);
		printf("Saving IP to cache...\n");
		strcat(cacheDir, HOST);
		fd = open(cacheDir, O_WRONLY | O_CREAT, 0666); // name file based off of HOST
		write(fd, IP, strlen(IP));
		close(fd);

		if (connect(sock, remote_addr->ai_addr, remote_addr->ai_addrlen) < 0) {
			printError(Failed to connect to remote server);
			return -1;
		}
	} else { // else read IP from cache
		printf("Opening IP address from cache for %s\n", HOST);
		char IP[INET_ADDRSTRLEN]; // max length of IPv4 is 15 characters

		strcat(cacheDir, HOST);
		fd = open(cacheDir, O_RDONLY);
		read(fd, IP, INET_ADDRSTRLEN);
		close(fd);
		printf("%s -> %s\n", HOST, IP);

		printf("Checking if IP is on blacklist\n");
		if (on_blacklist(IP)) {
			printf("IP is blacklisted\n");
			return -1;
		}

		// create sockaddr_in structure to connect to remote server
		struct sockaddr_in remote;
		bzero(&remote, sizeof(remote));
		remote.sin_family = AF_INET;
		remote.sin_port = htons(80); // connect to port 80 for HTTP requests
		remote.sin_addr.s_addr = inet_addr(IP);

		if (connect(sock, (struct sockaddr*)&remote, sizeof(remote)) < 0) {
			printError(Failed to connect to remote server);
			return -1;
		}
	}

	return sock;
}

/* This function takes a HOST name and determines if it is located in cache */
int IPcached(char* HOST) {
	char cacheDir[200] = "./cache/";

	strcat(cacheDir, HOST);

	if (access(cacheDir, F_OK) == 0) return 1;// if file exists in cache return 1
	return 0;
}


/* This function forwards data from the source socket to the destination socket and is the core function as a proxy */
void forwardData(int dest_sock, int src_sock, char* host, char* path, int remote_flag) {
	ssize_t bytes;

	char buffer[MAXBUFSIZE];

	if (remote_flag) { // if forwarding from remote to client capture data and place in cache
		int fd;
		char filename[200] = {0};
		char hash[MD5_DIGEST_LENGTH * 2 + 1];

		// construct filename based on requested file (path), from host
		strcat(filename, host);
		strcat(filename, "_");
		strcat(filename, path);
		//printf("filename before hash: %s\n", filename);

		// create hash of filename to use to create file in cache directory
		fntohash(hash, filename);

		// file is located in cache directory (./cache/<hash>)
		memset(filename, 0, sizeof(filename));
		strcpy(filename, "./cache/");
		strcat(filename, hash);

		if ((fd = open(filename, O_WRONLY | O_CREAT, 0666)) < 0) {
			printError(Could not create a cache file);
		}

		printf("Forwarding data...\n");
		int i = 1;
		while ((bytes = recv(src_sock, buffer, MAXBUFSIZE, 0)) > 0) { // read data from source socket
			printf("Sending packet %d containing %d bytes\n", i, (int)bytes);
			send(dest_sock, buffer, bytes, 0); // send data to destination socket
			printf("Writing packet to %s\n", filename);
			write(fd, buffer, bytes); // write data to cache file
			i++;
		}
		printf("Finished\n");

		if (bytes < 0) {
			printError(Connection was lost);
		}
	} else { // just forward data if going from client to remote
		printf("Forwarding data...\n");
		bytes = recv(src_sock, buffer, MAXBUFSIZE, 0); // read data from source socket
		send(dest_sock, buffer, bytes, 0); // send data to destination socket

		printf("Finished\n");

		if (bytes < 0) {
			printError(Connection was lost);
		}
	}
}

/* This function determines if the requested file on path from host is in cache and within timeout (present = 1, not present = 0) */
int cached_and_timeout(char* host, char* path, int tout) {
	char filename[200] = {0};
	char hash[MD5_DIGEST_LENGTH * 2 + 1];
	time_t rawtime;
	struct tm* currentTime, * fileTime;
	struct stat fileinfo;
	int ltime, atime;

	// grab localtime and change it to something useful <HHMMSS>
	time(&rawtime);
	currentTime = localtime(&rawtime);
	ltime = ((currentTime->tm_hour) << 12) | ((currentTime->tm_min)<<6) | (currentTime->tm_sec);
	printf("Local time: %d:%d:%d\n", currentTime->tm_hour, currentTime->tm_min, currentTime->tm_sec);

	// build filename from path and host requested by client
	strcat(filename, host);
	strcat(filename, "_");
	strcat(filename, path);

	// create hash of filename to use to create file in cache directory
	fntohash(hash, filename);

	// file is located in cache directory (./cache/<hash>)
	memset(filename, 0, sizeof(filename));
	strcpy(filename, "./cache/");
	strcat(filename, hash);

	// grab metadata of file for timeout purposes
	if (stat(filename, &fileinfo) < 0) {
		printError(Could not read file metadata);
	}

	// convert last access time into usable format <HHMMSS>
	fileTime = localtime(&fileinfo.st_atime);
	atime = ((fileTime->tm_hour) << 12) | ((fileTime->tm_min)<<6) | (fileTime->tm_sec);
	printf("Last access time: %d:%d:%d\n", fileTime->tm_hour, fileTime->tm_min, fileTime->tm_sec);

	printf("Checking for file: %s...\n", filename);

	if (access(filename, F_OK) == 0) { // if file exists return 1
		printf("Found\n");
		// calculate difference between local time and last access time of file
		int lastAccess = ltime - atime;
		if (0 < lastAccess && lastAccess < tout) {
			printf("File is within timeout\n");
			return 1;
		}
		printf("File is outside timeout\n");
		return 0;
	}
	printf("Not found\n");
	return 0;
}

/* This function converts a filename to a md5 hash (assumes file is located inside cache directory) */
void fntohash(char* hash, char* filename) {
	char newfilename[MD5_DIGEST_LENGTH];
	memset(newfilename, 0, sizeof(newfilename));

	MD5((unsigned char*)filename, strlen(filename), (unsigned char*)newfilename);

	for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        sprintf(hash + 2 * i, "%02x", newfilename[i]);
	}

	return;
}

/* This function checks if the requested host is in the blacklist file (on blacklist = 1, not on blacklist = 0) */
int on_blacklist(char* HOST) {
	int fd;
	char* token, buffer[MAXBUFSIZE];

	fd = open("blacklist", O_RDONLY);
	read(fd, buffer, MAXBUFSIZE);
	close(fd);

	token = strtok(buffer, "\n");
	while (token != NULL) {
		if (strcmp(HOST, token) == 0) return 1;
		token = strtok(NULL, "\n");
	}
	return 0;
}

