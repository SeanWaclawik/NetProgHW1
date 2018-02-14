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
#define ERRSIZE 32

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

char error_messages[8][ERRSIZE] = {
	"Not defined\0", 
	"File not found\0", 
	"Access violation\0", 
	"Disk full\0", 
	"Illegal operation\0", 
	"Unknown port\0", 
	"File already exists\0",
	"No such user\0"
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

/* sends ACK packet to the specified address via sendto() */
void send_acknowledgement(int sockfd, unsigned short int block, 
				struct sockaddr_in* serveraddr, socklen_t sockaddr_len) {
	
	char buffer[4];
	unsigned short int opcode = htons(ACK);
	block = htons(block);
	int i;
	
	/* construct packet */
	memset(&buffer, 0, BUFSIZE);
	
	memcpy(buffer, &opcode, 2);		// opcode (first two bytes)
	memcpy(buffer+2, &block, 2);	// block# (third & fourth bytes)

	/* send packet */
	i = sendto(sockfd, buffer, 4, 0, (struct sockaddr*) serveraddr, sockaddr_len);
	if (i < 0) {
		info(getpid(), "sendto() error\n");
		perror("sendto()");
		exit(-1);
	}
}

/* sends ERROR packet to the specified address via sendto() */
int send_error(int sockfd, unsigned short int block, unsigned short int error_code,
				struct sockaddr_in* serveraddr, socklen_t sockaddr_len) {
	
	char buffer[BUFSIZE];
	unsigned short int opcode = htons(ERROR);
	block = htons(block);
	
	char *error_msg = error_messages[error_code];

	/* construct packet */
	memset(&buffer, 0, BUFSIZE);
	
	memcpy(buffer, &opcode, 2);			// opcode (first two bytes)
	memcpy(buffer+2, &block, 2);		// block# (third & fourth bytes)
	memcpy(buffer+4, &error_msg, ERRSIZE);	// error message (additional 0-512 bytes)

	/* send packet */
	return sendto(sockfd, buffer, sizeof(error_msg)+4, 0, (struct sockaddr*) serveraddr, sockaddr_len);
}

/* sends DATA packet to the specified address via sendto() */
void send_data(int sockfd, unsigned short int block, char data[], int size,
				struct sockaddr_in* serveraddr, socklen_t sockaddr_len) {

	char buffer[BUFSIZE];
	unsigned short int opcode = htons(DATA);
	block = htons(block);
	int i;
	
	/* construct packet */
	memset(&buffer, 0, BUFSIZE);

	memcpy(buffer, &opcode, 2);		// opcode (first two bytes)
	memcpy(buffer+2, &block, 2);	// block# (third & fourth bytes)
	memcpy(buffer+4, &data, size);	// data   (additional 0-512 bytes)

	/* send packet */
	i = sendto(sockfd, buffer, size+4, 0, (struct sockaddr*) serveraddr, sockaddr_len);
	if (i < 0) {
		info(getpid(), "sendto() error\n");
		perror("sendto()");
		exit(-1);
	}
}


/* handles tftp server RRQ opcode */
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

	/* error, file not found */
	if(fp == NULL) { 					
		errcode = FILE_NOT_FOUND;
		n = send_error(sockfd, block, errcode, pc_info->serveraddr, pc_info->server_len);
		if(n < 0) {
			free(fname);
			perror("child: RRQ send_error()");
			exit(-1);
		}
		free(fname);
		perror("child: RRQ fopen()");
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
		errcode = ILLEGAL_OP;
		n = send_error(sockfd, block, errcode, pc_info->serveraddr, pc_info->server_len);
		if(n < 0) {
			perror("child: RRQ sendto()");
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

/* handles tftp server WRQ opcode */
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
	unsigned short int opcode, errcode, block = 0, prevblock;

	/* get file name from the request */
	char *fname = calloc(128, sizeof(char));

	for(i = 2; i < 512; i++) {
		fname[i-2] = buffer[i];
		if (buffer[i] == '\0') break;
	}

	/* open file to write */
	FILE *fp = fopen(fname, "w");
	if(fp == NULL) {

		/* log and send error message to client */
		perror("child: WRQ fopen()");
		errcode = NOT_DEFINED;
		n = send_error(sockfd, block, errcode, pc_info->serveraddr, pc_info->server_len);
		if (n < 1) {
			perror("child: WRQ fopen() send_error()");
			free(fname);
			exit(-1);
		}
		free(fname);	
		exit(-1);
	}
	free(fname);

	/* send ack to client, then start copying data */
	send_acknowledgement(sockfd, 0, pc_info->serveraddr, pc_info->server_len);
	prevblock = 0;

	/* receive data until n < 516 */
	do {

<<<<<<< HEAD

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
				send_acknowledgement(sockfd, 0, pc_info->serveraddr, pc_info->server_len);
			}
			else {
				// we got our a response, reset timer
				time(&time1);
			}

		} while (n < 0 && elapsed < ABORT_TIMEOUT);
		if(n < 0 || elapsed >= ABORT_TIMEOUT) {
			perror("ERROR: recvfrom() abort timeout");
=======
		n = recvfrom(sockfd, buffer, BUFSIZE, 0, (struct sockaddr*) pc_info->childaddr, &(pc_info->child_len));
		if(n < 0) {
			perror("recvfrom()");
>>>>>>> e306ccdaed08d58581f03b70fbce691f4ee0a13c
			exit(-1);
		}
		// we got our a response, reset timer
		time(&time1);



		
		/* first two bytes should be DATA opcode */
		opcode_ptr = (unsigned short int*) buffer;
		opcode = ntohs (*opcode_ptr);
		
		if(opcode != DATA) {
			/* log and send error message to client */
			printf("error: did not receive DATA opcode\n");
			errcode = ILLEGAL_OP;
			n = send_error(sockfd, block, errcode, pc_info->serveraddr, pc_info->server_len);
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
	} while ( n == BUFSIZE && elapsed < ABORT_TIMEOUT);

	if (elapsed >= ABORT_TIMEOUT){
		// tried too many times, kill connection
		perror("child: exceeded max timeout; killing connection");
		exit(-1);
	}


	// info(getpid(), "done getting data\n");
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