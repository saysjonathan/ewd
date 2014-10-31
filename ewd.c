#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAXBUFLEN 1024
#define MAXJOBS USHRT_MAX

typedef struct job job;
typedef struct queue queue;

struct job {
	char *args;
	job *next;
};

struct queue {
	char *name;
	int child;
	int maxchild;
	char *cmd;
	queue *next;
	job *head, *tail;
};

static char *config = "/etc/ewd.conf";
static int masterfd = -1;
static char *port = "8277";
static queue *queues = NULL;
volatile sig_atomic_t running = 1;

static int queuesize(queue *q) {
	job *j;
	int size = 0;
	j = q->head;
	while(j) {
		size++;
		j = j->next;
	}
	return size;
}

static char *dequeue(queue *q) {
	job *j;
	char *s = NULL;
	if(!q->head) {
		return s;
	}
	j = q->head;
	s = j->args;
	if (q->head == q->tail) {
		q->head = q->tail = NULL;
	} else {
		q->head = j->next;
	}
	free(j);
	return(s);
}

static int enqueue(queue *q, char *jargs) {
	job *j;
	j = malloc(sizeof(job));
	if(!j) {
		fprintf(stderr, "cannot allocate memory: %s\n", strerror(errno));
		return -1;
	}
	if(!q->head) {
		q->head = q->tail = j;
	} else {
		q->tail->next = j;
		q->tail = j;
	}
	j->args = jargs;
	j->next = NULL;
	return 0;
}

static queue *findqueue(char *name) {
	queue *q;
	for(q = queues; q; q = q->next) {
		if(strcmp(q->name, name) == 0) {
			return q;
		}
	}
	q = NULL;
	return q;
}

static void rmqueue(queue *q) {
	queue *t;
	if(queues == q) {
		queues = q->next;
	} else {
		for(t = queues; t; t = t->next) {
			if(t->next == q) {
				t->next = q->next;
			}
		}
	}
}

static void mkqueue(char *qname, int qmax, char *qcmd) {
	queue *q;
	for(q = queues; q; q = q->next) {
		if(!strcmp(qname, q->name)) {
			printf("Queue %s already exists\n", qname);
		}
	}
	q = malloc(sizeof(queue));
	if(!q) {
		fprintf(stderr, "cannot allocate memory: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	q->name = qname;
	q->maxchild = qmax;
	q->cmd = qcmd;
	q->child = 0;
	q->next = queues;
	q->head = q->tail = NULL;
	queues = q;
}

static int mkchild(queue *q) {
	int p;
	char *a;
	a = dequeue(q);
	if(a) {
		char *args[] = {q->cmd, a, NULL};
		p = fork();
		if(p == 0) {
			execv(q->cmd, args);
			exit(EXIT_SUCCESS);
		} else if(p == -1) {
			fprintf(stderr, "unable to fork: %s\n", strerror(errno));
		}
	}
	return 1;
}

static int parsereq(char *req) {
	queue *q;
	char *col, *name, *args;
	col = strsep(&req, " \t");
	if(!col) {
		fprintf(stderr, "error: unable to parse `name` field in quest\n");
		return -1;
	}
	name = strdup(col);
	col = strsep(&req, "\n");
	if(!col) {
		fprintf(stderr, "error: missing `cmd` field\n");
		return -1;
	}
	args = strdup(col);
	if(!(q = findqueue(name)) != 0) {
		fprintf(stderr, "unable to find queue: %s\n", name);
		return -1;
	}
	if(enqueue(q, args) != 0) {
		fprintf(stderr, "unable to add job to queue: %s\n", args);
		return -1;
	}
	return 0;
}

static int handle(int fd) {
	char buf[MAXBUFLEN];
	int rfd, reqlen;
	socklen_t salen;
	struct sockaddr sa;
	rfd = accept(fd, &sa, &salen);
	if (rfd < 0) {
		fprintf(stderr, "cannot accept request: %s\n", strerror(errno));
		close(rfd);
		return -1;
	}
	reqlen = recv(rfd, buf, MAXBUFLEN-1, 0);
	if (reqlen < 0) {
		fprintf(stderr, "recv error: %s\n", strerror(errno));
		close(rfd);
		return -1;
	}
	if (parsereq(buf) < 0) {
		fprintf(stderr, "unable to parse req: %s\n", buf);
		close(rfd);
		return -1;
	}
	close(rfd);
	return 0;
}

static int poll(fd_set fd) {
	int ret;
	struct timeval tv;
	tv.tv_sec = 2;
	tv.tv_usec = 500000;
	ret = select(masterfd+1, &fd, NULL, NULL, &tv);
	if(ret == -1) {
		fprintf(stderr, "unable to select: %s\n", strerror(errno));
	}
	if(FD_ISSET(masterfd, &fd)) {
		if(handle(masterfd) != 0) {
			fprintf(stderr, "unable to handle request\n");
		}
	}
	return 0;
}

static void master(queue *q, int fd) {
	int s;
	fd_set master;
	FD_SET(fd, &master);
	while(running) {
		for(q = queues; q; q = q->next) {
			if(q->child > 0) {
				while(waitpid(-1, &s, WNOHANG) > 0) {
					q->child--;
				}
			}
			while((q->child < q->maxchild) && queuesize(q) > 0) {
				q->child += mkchild(q);
				continue;
			}
		}
		if(!poll(master)) {
			continue;
		}
	}
	killpg(0, SIGHUP);
}

static int tcpopen(const char *port) {
        struct addrinfo hints, *ai = NULL;
	int status;
        int fd = -1;

        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        if ((status = getaddrinfo(NULL, port, &hints, &ai)) < 0) {
                fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
		if (fd >= 0) {
			close(fd);
		}
		if (ai) {
			freeaddrinfo(ai);
		}
                exit(EXIT_FAILURE);
        }
        if ((fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0) {
                fprintf(stderr, "socket error: %s\n", gai_strerror(fd));
		if (fd >= 0) {
			close(fd);
		}
		if (ai) {
			freeaddrinfo(ai);
		}
                exit(EXIT_FAILURE);
        }
        if ((status = bind(fd, ai->ai_addr, ai->ai_addrlen)) < 0) {
                fprintf(stderr, "bind error: %s\n", gai_strerror(fd));
		if (fd >= 0) {
			close(fd);
		}
		if (ai) {
			freeaddrinfo(ai);
		}
                exit(EXIT_FAILURE);
        }
        if ((status = listen(fd, SOMAXCONN)) < 0) {
                fprintf(stderr, "listen error: %s\n", gai_strerror(fd));
		if (fd >= 0) {
			close(fd);
		}
		if (ai) {
			freeaddrinfo(ai);
		}
                exit(EXIT_FAILURE);
        }

        freeaddrinfo(ai);
        return fd;
}

static void sighandler(int sig) {
	switch(sig) {
		case SIGINT:
		case SIGTERM:
			close(masterfd);
			running = 0;
			break;
		default:
			break;
	}
}

static int setuphandlers(void) {
	struct sigaction sa;
	memset(&sa, 0x0, sizeof(sa));
	sa.sa_handler = sighandler;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGINT, &sa, NULL) < 0) {
		fprintf(stderr, "could not reset SIGINT handler");
		return 1;
	}
	if (sigaction(SIGTERM, &sa, NULL) < 0) {
		fprintf(stderr, "could not reset SIGTERM handler");
		return 1;
	}
	return 0;
}

static void cleanup(queue *q) {
	job *j;
	for(q = queues; q; q = q->next) {
		j = q->head;
		while(j) {
			j = j->next;
		}
		if(queuesize(q) == 0) {
			rmqueue(q);
		} else {
			printf("Unable to remove queue %s\n", q->name);
		}
	}
}

static int parsemaxwrks(const char *s) {
	long l;
	char *e;
	l = strtol(s, &e, 10);
	if(e[0]) {
		return -1;
	}
	return (int)l;
}

static int loadqueues(char *cf) {
	FILE *f;
	char *line = NULL, *p, *col, *name, *cmd;
	int r = 0, l, max;
	size_t size = 0;
	size_t len;
	if(!(f = fopen(cf, "r"))) {
		fprintf(stderr, "unable to open config: %s\n", cf);
		return -1;
	}
	for(l = 0; (len = getline(&line, &size, f)) != -1; l++) {
		p = line;
		if(line[0] == '#' || line[0] == '\n' || line[0] == '\0') {
			continue;
		}

		col = strsep(&p, " \t");
		if(!col) {
			fprintf(stderr, "error: failed to parse `name` field on line %d\n", l+1);
			r = -1;
			break;
		}
		name = strdup(col);

		col = strsep(&p, " \t");
		if(!col || (max = parsemaxwrks(col)) < 0) {
			fprintf(stderr, "error: failed to parse `maxworkers` on line %d\n", l+1);
			r = -1;
			break;
		}

		col = strsep(&p, "\n");
		if(!col) {
			fprintf(stderr, "error: missing `cmd` field on line %d\n", l+1);
			r = -1;
			break;
		}
		cmd = strdup(col);
		mkqueue(name, max, cmd);
	}
	free(line);
	fclose(f);
	return r;
}

static void usage(void) {
	fprintf(stderr, "ewd "VERSION"\n"
			"usage: ewd [-c config] [-p port] [-h]\n");
	exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
	int i;
	while ((i = getopt(argc, argv, "c:p:h")) != -1) {
		switch(i) {
			case 'c':
				config = optarg;
				break;
			case 'p':
				port = optarg;
			case 'h':
			default:
				usage();
		}
	}
	if(setuphandlers()) {
		fprintf(stderr, "unable to setup signal handlers\n");
		exit(EXIT_FAILURE);
	}
	if(loadqueues(config) < 0) {
		fprintf(stderr, "unable to load queues\n");
	}
	if(!queues) {
		exit(EXIT_FAILURE);
	}
	masterfd = tcpopen(port);
	master(queues, masterfd);
	close(masterfd);
	cleanup(queues);
	fprintf(stderr, "exiting\n");
	exit(EXIT_SUCCESS);
}
