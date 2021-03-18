#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>

#define _GNU_SOURCE
#include <sys/types.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/ioctl.h>

#define errExit(msg)    do { perror(msg); exit(EXIT_FAILURE);	\
	} while (0)

#define BUFF_SIZE 4096

static int page_size;
static int pages;
static int page;
static int* msi;
static int port;
static int remote_port; 
static int server_fd, new_socket;
static struct sockaddr_in address;
static struct sockaddr_in serv_addr;
static int opt = 1;
static int addrlen = sizeof(address);
static int sock = 0;
static char *addr; 
static char cause;
static char* user_msg;
static pthread_mutex_t lock;
static struct output {
		int state;
		char s[4096];
		unsigned long len;
	};
static struct input {
		int page;
		char op;
	};
static enum state {I, S, M};

static void *
fault_handler_thread(void *arg)
{
	static struct uffd_msg msg;   /* Data read from userfaultfd */
	static int fault_cnt = 0;     
	long uffd;                    /* userfaultfd file descriptor */
	static char *page_addr = NULL;
	struct uffdio_copy uffdio_copy;
	ssize_t nread;

	uffd = (long) arg;

	if (page_addr == NULL) {
		page_addr = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (page_addr == MAP_FAILED)
			errExit("mmap");
	}

	for (;;) {

		struct pollfd pollfd;
		int nready;
	
		pollfd.fd = uffd;
		pollfd.events = POLLIN;
		nready = poll(&pollfd, 1, -1);
		if (nready == -1)
			errExit("poll");
		pthread_mutex_lock(&lock);

		nread = read(uffd, &msg, sizeof(msg));
		if (nread == 0) {
			printf("EOF on userfaultfd!\n");
			exit(EXIT_FAILURE);
		}

		if (nread == -1)
			errExit("read");

		if (msg.event != UFFD_EVENT_PAGEFAULT) {
			fprintf(stderr, "Unexpected event on userfaultfd\n");
			exit(EXIT_FAILURE);
		}

		printf("  [x]  PAGEFAULT\n");

		memset(page_addr, 0, page_size);
		uffdio_copy.src = (unsigned long) page_addr;
		uffdio_copy.dst = (unsigned long) msg.arg.pagefault.address &
			~(page_size - 1);
		uffdio_copy.len = page_size;
		uffdio_copy.mode = 0;
		uffdio_copy.copy = 0;
		if (ioctl(uffd, UFFDIO_COPY, &uffdio_copy) == -1) {
			printf("copying failed\n");
			errExit("ioctl-UFFDIO_COPY");
		}

		if(cause=='r') {
			printf("Current page is invalid. Sending message to get status from remote\n");
			struct input* send_buf = (struct input*)malloc(sizeof(struct input));
			send_buf->page = page;
			send_buf->op='r';
			send(sock , send_buf, sizeof(struct input), 0 );
			struct output* recieve_buf = (struct output*)malloc(sizeof(struct output));
			if (read( sock , recieve_buf, sizeof(struct output)) < 0) {
				printf("\nRead Failed \n");
				return -1;
		    }
		    char* cur_addr = addr+(page_size*page);
		    printf("State recieved: %d\n", recieve_buf->state);
		    if (recieve_buf->state == M) {
		    	printf("Recieved message: %s\n", recieve_buf->s);
		    	msi[page]=S;
		    	//write to the page
				int i=0;
				while(recieve_buf->s[i]!='\0') {
					printf("%c", recieve_buf->s[i]);
					cur_addr[i] = recieve_buf->s[i];
					i++;
				}
				cur_addr[i]='\0';
		    }
		}
		pthread_mutex_unlock(&lock);
	}
}

static void * communication_thread(void* arg) {
	while(1) {
		struct input* in = (struct input*)malloc(sizeof(struct input));

		if (read( new_socket , in, sizeof(struct input)) < 0) {
			perror("accept");
			exit(EXIT_FAILURE);
		}
		printf("Recieved message - Need msi state for page: %d\n", in->page);
		struct output* out = (struct output*)malloc(sizeof(struct output)); 
		out->state = msi[in->page];
		if(in->op=='r') {
			if(out->state==M) {
				msi[in->page]=S;
				//read the page and send it.
				printf("sending page: %d\n", page);
				char* cur_addr = addr+(page_size*page);
				printf("[*]  Page  %i:\n", page);
				int l=0;
				while(cur_addr[l]!='\0') {
					out->s[l] = cur_addr[l];
					l++;
				}
				out->s[l]='\0';
			}
		}

		else if(in->op=='w') {
				msi[in->page]=I;
				char* cur_addr = addr+(page_size*page);

				if (madvise(cur_addr, page_size, MADV_DONTNEED)) {
					errExit("fail to madvise");
				}

				printf("Page %d invalidated\n", in->page);
				//read the page and send it.
				out->state = I;
		}

		send(new_socket , out , sizeof(struct output), 0);
		printf("message sent\n");
	}
}

int main(int argc, const char *argv[]) {
	port = atoi(argv[1]);
	remote_port = atoi(argv[2]);
	
	unsigned long len;  
	struct message {
		char* addr;
		unsigned long size;
		unsigned int pages;
	};
	long uffd;          /* userfaultfd file descriptor */
	struct uffdio_api uffdio_api;
	struct uffdio_register uffdio_register;
	int l;
	char* cur_addr;
	pthread_t thr;
	pthread_t com_thr;
	 
	struct msi_msg {
		int page;
	};

	memset(&serv_addr, '0', sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(remote_port);

	page_size = sysconf(_SC_PAGE_SIZE);

	
	if(inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
		printf("\nInvalid address/ Address not supported \n");
		return -1;
	}

	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		printf("\n Socket creation error \n");
		return -1;
	}

	if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {

		if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
			perror("socket failed");
			exit(EXIT_FAILURE);
		}

		if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
			       &opt, sizeof(opt))) {
			perror("setsockopt");
			exit(EXIT_FAILURE);
		}

		address.sin_family = AF_INET;
		address.sin_addr.s_addr = INADDR_ANY;
		address.sin_port = htons( port );

		if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
			perror("bind failed");
			exit(EXIT_FAILURE);
		}

		if (listen(server_fd, 3) < 0) {
			perror("listen");
			exit(EXIT_FAILURE);
		}
		if ((new_socket = accept(server_fd, (struct sockaddr *)&address,
					 (socklen_t*)&addrlen)) < 0) {
			perror("accept");
			exit(EXIT_FAILURE);
		}

		printf("Please enter the number of pages...\n");
		char* input = malloc(sizeof(char)*10);
		fgets(input, 10, stdin);
		pages = atoi(input);
		len = pages * page_size;

		addr = mmap(NULL, len, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (addr == MAP_FAILED)
			errExit("mmap");

		printf("Address returned by mmap() = %p\n", addr);
		printf("Size allocated for request = %lu\n", len);

		struct message* msg = (struct message*)malloc(sizeof(struct message));
		msg->size = len;
		msg->addr = addr;
		msg->pages=pages;
		connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)); 
		send(sock , msg , sizeof(struct message), 0 );
		printf("message sent\n");
	}
	else {
		if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
			perror("socket failed");
			exit(EXIT_FAILURE);
		}

		if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
			       &opt, sizeof(opt))) {
			perror("setsockopt");
			exit(EXIT_FAILURE);
		}

		address.sin_family = AF_INET;
		address.sin_addr.s_addr = INADDR_ANY;
		address.sin_port = htons( port );

		if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
			perror("bind failed");
			exit(EXIT_FAILURE);
		}

		if (listen(server_fd, 3) < 0) {
			perror("listen");
			exit(EXIT_FAILURE);
		}

		if ((new_socket = accept(server_fd, (struct sockaddr *)&address,
					 (socklen_t*)&addrlen)) < 0) {
			perror("accept");
			exit(EXIT_FAILURE);
		}


		struct message* buffer = (struct message*)malloc(sizeof(struct message));

		if (read( new_socket , buffer, sizeof(struct message)) < 0) {
			perror("accept");
			exit(EXIT_FAILURE);
		}

		addr = mmap(buffer->addr, buffer->size, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (addr == MAP_FAILED)
			errExit("mmap");
		len=buffer->size;
		pages=buffer->pages;
		printf("Address returned by mmap() = %p\n", addr);
		printf("Size allocated for request = %lu\n", len);	
	}


	uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (uffd == -1)
		errExit("userfaultfd");
	uffdio_api.api = UFFD_API;
	uffdio_api.features = 0;

	if (ioctl(uffd, UFFDIO_API, &uffdio_api) == -1)
		errExit("ioctl-UFFDIO_API");

	uffdio_register.range.start = (unsigned long) addr;
	uffdio_register.range.len = len;
	uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
	if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1)
		errExit("ioctl-UFFDIO_REGISTER");
	int s = pthread_create(&thr, NULL, fault_handler_thread, (void *) uffd);
	if (s != 0) {
		errno = s;
		errExit("pthread_create");
	}

	msi = calloc(I, sizeof(int)*pages);
	void* arg=NULL;

	int p = pthread_create(&com_thr, NULL, communication_thread, arg);
	if (p != 0) {
		errno = p;
		errExit("pthread_create");
	}

	while(1) {
		printf("Which command should I run? (r:read, w:write, v:view msi array): \n");
		char* cmd = malloc(sizeof(char)*10);
		fgets(cmd, 10, stdin);
		if ((strlen(cmd) > 0) && (cmd[strlen (cmd) - 1] == '\n'))
    		cmd[strlen (cmd) - 1] = '\0';

		
		l=0x0;
		if(!strcmp(cmd, "r")) {
			printf("For which page? (0-%i, or -1 for all):", pages-1);
			char* in = malloc(sizeof(char)*10);
			fgets(in, 10, stdin);
			page = atoi(in);
			printf("Running command: %s for page: %d\n", cmd, page);

			cause='r';
			if(page!=-1) {
				cur_addr = addr+(page_size*page);
				printf("[*]  Page  %i:\n", page);
				while(l<page_size) {
					char c = cur_addr[l];
					printf("%c", c);
					l++;
				}
				// sleep(2);
				pthread_mutex_lock(&lock);
				pthread_mutex_unlock(&lock);
				if(msi[page]==I) {
					if (madvise(cur_addr, page_size, MADV_DONTNEED)) {
						errExit("fail to madvise");
					}
					printf("madvise called\n");
				}
			}
			else{
				cur_addr = addr;
				l=0;
				while(l<pages) {
					page=l;
					printf("[*]  Page  %i:\n", l);
					cur_addr = addr+(page_size*l);
					printf("%s\n", cur_addr);
					l++;
					// sleep(2);
					pthread_mutex_lock(&lock);
					pthread_mutex_unlock(&lock);
					if(msi[page]==I) {
						if (madvise(cur_addr, page_size, MADV_DONTNEED)) {
							errExit("fail to madvise");
						}
						printf("madvise called\n");
					}
				}
				
			}
			printf("\n");
		}
		else if(!strcmp(cmd, "w")) {
			printf("For which page? (0-%i, or -1 for all):", pages-1);
			char* in = malloc(sizeof(char)*10);
			fgets(in, 10, stdin);
			page = atoi(in);
			printf("Running command: %s for page: %d\n", cmd, page);

			cause='w';
			printf("Please enter the message\n");
			user_msg=malloc(sizeof(char)*4095);
			fgets(user_msg, 1000, stdin);
			if(page!=-1) {
				cur_addr = addr+(page_size*page);
				int i=0;
				while(user_msg[i]!='\n') {
					cur_addr[i] = user_msg[i];
					i++;
				}
				cur_addr[i]='\0';
				// sleep(1);
				pthread_mutex_lock(&lock);
				pthread_mutex_unlock(&lock);
				if(msi[page]==S) {
					struct input* send_buf = (struct input*)malloc(sizeof(struct input));
					send_buf->page = page;
					send_buf->op='w';
					send(sock , send_buf, sizeof(struct input), 0);
				}
				msi[page]=M;
			}
			else{
				cur_addr = addr;
				l=0;
				while(l<pages) {
					page=l;
					cur_addr = addr+(page_size*l);
					int i=0;
					while(i<strlen(user_msg)) {
						cur_addr[i] = user_msg[i];
						i++;
					}
					cur_addr[i]='\0';
					// sleep(1);
					pthread_mutex_lock(&lock);
					pthread_mutex_unlock(&lock);
					if(msi[page]==S) {
						struct input* send_buf = (struct input*)malloc(sizeof(struct input));
						send_buf->page = page;
						send_buf->op='w';
						send(sock , send_buf, sizeof(struct input), 0);
					}
					msi[page]=M;
					l++;
				}
			}
			printf("Message written to page/s\n");
		}
		else if(!strcmp(cmd, "v")) {
			int i=0;
			printf("msi array:");
			char c;
			for(i=0; i<pages; i++) {
				if(msi[i]==0) {
					c='I';
				}
				else if(msi[i]==1) {
					c='S';
				}
				else {
					c='M';
				}
				printf(" %c", c);
			}
			printf("\n");
		}
	}
	
	return 0;
}