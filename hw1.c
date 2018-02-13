#include <stdio.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <assert.h>
#include <time.h>
#include <sys/time.h>

#define BUFSIZE 516
#define RETRANS_TIMEOUT 1 // seconds
#define ABORT_TIMEOUT 10 // seconds

enum opcode {
	RRQ = 1,		// Read Request
	WRQ = 2,		// Write Request
	DATA = 3,		// Data
	ACK = 4,		// Acknowledgement
	ERROR = 5		// Error
};

enum errcode {
	NOT_DEFINED = 			0,
	FILE_NOT_FOUND = 		1,
	ACCESS_VIOLATION = 		2,
	DISK_FULL = 			3,
	ILLEGAL_OP = 			4,
	UNKNOWN_PORT = 			5,
	FILE_ALREADY_EXISTS = 	6,
	NO_SUCH_USER =			7
};

/* parent/child socket info */
struct pc_socket_info {
	struct sockaddr_in *serveraddr;
	struct sockaddr_in *childaddr;
	socklen_t server_len;
	socklen_t child_len;
};

void info(int pid, char *s) {
	printf("[%d] %s", pid, s);
}

void send_acknowledgement(int sockfd, unsigned short int block, 
				struct sockaddr_in* serveraddr, socklen_t sockaddr_len) {
	
	char buffer[4];
	unsigned short int opcode = htons(ACK);
	unsigned short int zero = htons(0);
	block = htons(block);
	int i;
	
	memcpy(buffer, &opcode, 2);
	
	if(block == zero) memcpy(buffer+2, &zero, 2);
	else memcpy(buffer+2, &block, 2);

	i = sendto(sockfd, buffer, 4, 0, (struct sockaddr*) serveraddr, sockaddr_len);
	if (i < 0) {
		info(getpid(), "sendto() error\n");
		perror("sendto()");
		exit(-1);
	}
};

void send_data(int sockfd, unsigned short int block, char data[], int size,
				struct sockaddr_in* serveraddr, socklen_t sockaddr_len) {

	char buffer[BUFSIZE];
	unsigned short int opcode = htons(DATA);
	block = htons(block);
	int i;
	
	memset(&buffer, 0, BUFSIZE);

	memcpy(buffer, &opcode, 2);
	memcpy(buffer+2, &block, 2);
	memcpy(buffer+4, &data, size);

	i = sendto(sockfd, buffer, size+4, 0, (struct sockaddr*) serveraddr, sockaddr_len);
	if (i < 0) {
		info(getpid(), "sendto() error\n");
		perror("sendto()");
		exit(-1);
	}

}

void do_read_request(int sockfd, char buffer[], int size, struct pc_socket_info* pc_info) {
	
	// if our last contact was more than ABORT_TIMEOUT seconds we're going to kill it
	time_t time1, time2;
	double elapsed = 0.0;


	// Set the retransmit timer
	struct timeval tv;
	tv.tv_sec = RETRANS_TIMEOUT;
	tv.tv_usec = 0;


	// set the rcv socket to timeout
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) {
    	perror("Error: could not set timeout value for socket");
    	exit(-1);
	}
	
	// Start timer1 counting up
	time(&time1);

	info(getpid(), "Started timeout timers, entered read request\n");

	char sendbuf[BUFSIZE-4];

	unsigned short int *opcode_ptr;
	unsigned short int *block_ptr;
	unsigned short int opcode, errcode, block = 1, prevblock = 0;
	int i, n, copied = 0;
	char c;

	memset(&sendbuf, 0, BUFSIZE);

	/* get file name from the request */
	char *fname = calloc(128, sizeof(char));

	for(i = 2; i < 512; i++) {
		fname[i-2] = buffer[i];
		if (buffer[i] == '\0') break;
	}


	/* open file to write */
	FILE *fp = fopen(fname, "r");
	if(fp == NULL) { /* error, file not found */
		errcode = FILE_NOT_FOUND;
		opcode_ptr = (unsigned short int*) buffer;

		*opcode_ptr = htons(ERROR);
		*(opcode_ptr + 1) = htons(errcode);
		*(buffer + 4) = 0;

		n = sendto(sockfd, buffer, 5, 0, (struct sockaddr*) pc_info->serveraddr, 
							pc_info->server_len);
		if(n < 0) {
			free(fname);
			perror("child: sendto()");
			exit(-1);
		}
		free(fname);
		perror("child: fopen()");
		exit(-1);
	}
	free(fname);



newblock:

	/* send data to client */
	while (copied < BUFSIZE-4) {
		c = fgetc(fp);
		if (c == EOF) break;
		sendbuf[copied] = c;
		copied = copied+1;
	}

	sendbuf[copied] = '\0';
	send_data(sockfd, block, sendbuf, copied, pc_info->serveraddr, pc_info->server_len);
	
	do {
		// keep attempting to resend until our abort threshold is hit
		// get the current time
		time(&time2);
		// this gets the diff in seconds between time 2 and our last contact (time1)
		elapsed = difftime(time1, time2);

		n = recvfrom(sockfd, buffer, BUFSIZE, 0, (struct sockaddr*) pc_info->childaddr, &(pc_info->child_len));
		if (n < 0) {
			info(getpid(), "WARNING: recvfrom() timeout, trying resend..");
			// resend last
			send_data(sockfd, block, sendbuf, copied, pc_info->serveraddr, pc_info->server_len);
		}

	} while (n < 0 && elapsed < ABORT_TIMEOUT);
	if(n < 0 || elapsed >= ABORT_TIMEOUT) {
		perror("ERROR: recvfrom() abort timeout");
		exit(-1);
	}

	// else we made contact
	// reset our first timer
	time(&time1);


	/* first two bytes should be ACK opcode */
	opcode_ptr = (unsigned short int*) buffer;
	opcode = ntohs (*opcode_ptr);
	
	if(opcode != ACK) {
		printf("error: did not receive ACK opcode\n");
		*opcode_ptr = htons(ERROR);
		*(opcode_ptr + 1) = htons(4);
		*(buffer + 4) = 0;

		n = sendto(sockfd, buffer, 5, 0, (struct sockaddr*) pc_info->serveraddr, 
							pc_info->server_len);
		if(n < 0) {
			perror("child: sendto()");
			exit(-1);
		}
	}

	/* bytes 3 & 4 should be block number */
	block_ptr = (unsigned short int*) buffer;
	block_ptr = block_ptr+1;
	block = ntohs(*block_ptr);
	if(block != prevblock + 1) {
		printf("error: wrong block number\n");
	}

	printf("successfully sent block %d; copied %d bytes\n", block, copied);

	if(!feof(fp)) {
		copied = 0;
		prevblock = block;
		block += 1;
		goto newblock;
	} else {
		printf("end of file\n");
		if(copied == 512) { // send a final block of size 0 to end transmission
			block += 1;
			send_data(sockfd, block, sendbuf, 0, pc_info->serveraddr, pc_info->server_len);
		}
	}
	
	return;
}

void do_write_request(int sockfd, char buffer[], int size, struct pc_socket_info* pc_info) {
	// if our last contact was more than ABORT_TIMEOUT seconds we're going to kill it
	time_t time1, time2;
	double elapsed = 0.0;


	// Set the retransmit timer
	struct timeval tv;
	tv.tv_sec = RETRANS_TIMEOUT;
	tv.tv_usec = 0;

	
	// set the rcv socket to timeout
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) {
    	perror("Error: could not set timeout value for socket");
    	exit(-1);
	}
	
	// Start timer1 counting up
	time(&time1);

	info(getpid(), "Sarted timout timers, entered write request\n");
	int i, n = BUFSIZE;
	unsigned short int *opcode_ptr;
	unsigned short int *block_ptr;
	unsigned short int opcode, block, prevblock;

	/* get file name from the request */
	char *fname = calloc(128, sizeof(char));

	for(i = 2; i < 512; i++) {
		fname[i-2] = buffer[i];
		if (buffer[i] == '\0') break;
	}

	/* open file to write */
	FILE *fp = fopen(fname, "w");
	if(fp == NULL) {
		perror("child: fopen()");
		exit(-1);
	}

	/* send ack to client, then start copying data */
	send_acknowledgement(sockfd, 0, pc_info->serveraddr, pc_info->server_len);
	prevblock = 0;

	do {
		n = recvfrom(sockfd, buffer, BUFSIZE, 0, (struct sockaddr*) pc_info->childaddr, &(pc_info->child_len));
		if(n < 0) {
			perror("recvfrom()");
			exit(-1);
		}

		/* first two bytes should be DATA opcode */
		opcode_ptr = (unsigned short int*) buffer;
		opcode = ntohs (*opcode_ptr);
		
		if(opcode != DATA) {
			printf("error: did not receive DATA opcode\n");
			*opcode_ptr = htons(ERROR);
			*(opcode_ptr + 1) = htons(4);
			*(buffer + 4) = 0;

			n = sendto(sockfd, buffer, 5, 0, (struct sockaddr*) pc_info->serveraddr, 
								pc_info->server_len);
			if(n < 0) {
				perror("child: sendto()");
				exit(-1);
			}
		}


		/* bytes 3 & 4 should be block number */
		block_ptr = (unsigned short int*) buffer;
		block_ptr = block_ptr+1;
		block = ntohs(*block_ptr);
		if(block != prevblock + 1) {
			printf("error: data out of order\n");
		}

		/* data starts at byte 4 */
		for(i = 4; i < n+4; i++) {
			if(buffer[i] == '\0') { break; } 
			fprintf(fp, "%c", buffer[i]);
		}
		prevblock = block;
		send_acknowledgement(sockfd, block, pc_info->serveraddr, pc_info->server_len);
	} while ( n == BUFSIZE );


	info(getpid(), "done getting data\n");
	free(fname);
	fclose(fp);
	return;
}

void sig_child(int signo) {
	pid_t pid;
	int stat;

	while( (pid = waitpid(-1, &stat, WNOHANG)) > 0)
		printf("child %d terminated\n", pid);
}

int do_server() {
	char buffer[BUFSIZE];
	int sockfd, pid, n, optval;
	unsigned short int opcode;
	unsigned short int* opcode_ptr;
	// struct addrinfo *serverinfo;
	struct sockaddr_in serveraddr;
	struct sockaddr_in childaddr;
	socklen_t sockaddr_len;
	socklen_t child_len;
	
	struct pc_socket_info pc_info;
	// serverinfo->ai_family = AF_INET;
	// serverinfo->ai_socktype = SOCK_DGRAM;
	// serverinfo->ai_protocol = 0;

	pid = getpid();

	// set up socket
	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if(sockfd < 0) {
		perror("socket()");
		return EXIT_FAILURE;
	}

	optval = 1;
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
				(const void *)&optval , sizeof(int));

	// configure server (parent)
	sockaddr_len = sizeof(serveraddr);
	memset(&serveraddr, 0, sockaddr_len);
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serveraddr.sin_port = htons(0);
	serveraddr.sin_family = AF_INET;

	// configure child
	child_len = sizeof(childaddr);
	memset(&childaddr, 0, sockaddr_len);
	childaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	childaddr.sin_port = htons(0);
	childaddr.sin_family = AF_INET;

	// bind socket
	if((bind(sockfd, (struct sockaddr*) &serveraddr, sockaddr_len)) < 0) {
		perror("bind()");
		return EXIT_FAILURE;
	}

	getsockname(sockfd, (struct sockaddr*) &serveraddr, &sockaddr_len);
	printf("Port: %d\n", ntohs(serveraddr.sin_port));
	
	pc_info.serveraddr = &serveraddr;
	pc_info.childaddr = &childaddr;
	pc_info.server_len = sizeof(serveraddr);
	pc_info.child_len = sizeof(childaddr);
	while(1) {
		intr_recv:
			info(getpid(), "Awaiting new connection...\n"); 

			/* blocking call */ 
			n = recvfrom(sockfd, buffer, BUFSIZE, 0, (struct sockaddr*) &serveraddr, &sockaddr_len);
			if(n < 0) {
				if(errno == EINTR) goto intr_recv;
				perror("recvfrom()");
				return EXIT_FAILURE;
			}

			info(getpid(), "Received Connection...\n");

			//Check opcode
			opcode_ptr = (unsigned short int*) buffer;
			opcode = ntohs (*opcode_ptr);
			if(opcode != RRQ && opcode != WRQ)
			{
					*opcode_ptr = htons(ERROR);
					*(opcode_ptr + 1) = htons(4);
					*(buffer + 4) = 0;
				
			intr_send:
				n = sendto(sockfd, buffer, 5, 0, (struct sockaddr*) &serveraddr, sockaddr_len);
				if(n < 0)
				{
					if(errno == EINTR) goto intr_send;
					perror("sendto()");
					return EXIT_FAILURE;
				}
			}
			else
			{
				pid = fork();
				if(pid < 0)
				{
					perror("fork()");
					return EXIT_FAILURE;
				}
				// child handles request
				if(pid == 0)
				{
					close(sockfd);
					sockfd = socket(AF_INET, SOCK_DGRAM, 0);
					if(sockfd < 0) {
						perror("child: socket()");
						exit(-1);
					}

					if((bind(sockfd, (struct sockaddr*) &childaddr, child_len)) < 0) {
						perror("child: bind()");
						exit(-1);
					}
					break;
				}
			}
	}
	//Handle read and write requests
	if(opcode == RRQ) do_read_request(sockfd, buffer, n, &pc_info);
	if(opcode == WRQ) do_write_request(sockfd, buffer, n, &pc_info);
	close(sockfd);
	return EXIT_SUCCESS;
}

int main(int argc, char** argv)
{
	int exit_status = EXIT_SUCCESS;

	signal(SIGCHLD, sig_child);

	exit_status = do_server();

	return exit_status;
}