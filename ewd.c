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

typedef struct proc proc;
typedef struct job job;
typedef struct queue queue;

struct proc {
	int pid;
	proc *next;
};

struct job {
	char *args;
	job *next;
};

struct queue {
	char *name;
	int maxchild;
	char *cmd;
	queue *next;
	proc *child;
	job *head, *tail;
};

static char *config = "/etc/ewd.conf";
static int masterfd = -1;
static char *port = "8277";
static queue *queues = NULL;
volatile sig_atomic_t running = 1;

static int childlen(queue *q) {
	proc *c;
	int size = 0;
	c = q->child;
	while(c) {
		size++;
		c = c->next;
	}
	return size;
}

static int rmchild(queue *q, int pid) {
	proc *c, *pc;
	if(!q->child) {
		return -1;
	}
	if(q->child->pid == pid) {
		q->child = q->child->next;
		return 0;
	}
	for(c = q->child; c; pc = c, c = c->next) {
		if(c->pid == pid) {
			if(!c->next) {
				pc->next = NULL;
				return 0;
			} else {
				pc->next = c->next;
				break;
			}
		}
	}
	free(c);
	return 0;
}

static int addchild(queue *q, int pid) {
	proc *c;
	c = malloc(sizeof(proc));
	if(!c) {
		fprintf(stderr, "cannot allocate memory: %s\n", strerror(errno));
		return -1;
	}
	c->next = q->child;
	q->child = c;
	c->pid = pid;
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

static int queuelen(queue *q) {
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

static int enqueue(queue *q, char *args) {
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
	j->args = args;
	j->next = NULL;
	return 0;
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

static int mkqueue(char *name, int max, char *cmd) {
	queue *q;
	for(q = queues; q; q = q->next) {
		if(!strcmp(name, q->name)) {
			return 1;
		}
	}
	q = malloc(sizeof(queue));
	if(!q) {
		fprintf(stderr, "cannot allocate memory: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	q->name = name;
	q->maxchild = max;
	q->cmd = cmd;
	q->child = NULL;
	q->next = queues;
	q->head = q->tail = NULL;
	queues = q;
	return 0;
}

static int mkchild(queue *q) {
	pid_t p = 0;
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
	free(a);
	return p;
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
		free(name);
		return -1;
	}
	args = strdup(col);
	q = findqueue(name);
	if(enqueue(q, args) != 0) {
		fprintf(stderr, "unable to add job to queue: %s\n", args);
		free(name);
		free(args);
		return -1;
	}
	free(name);
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
	tv.tv_sec = 1;
	tv.tv_usec = 0;
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

static void master(int fd) {
	int s;
	fd_set master;
	queue *q;
	proc *c;
	FD_SET(fd, &master);
	while(running) {
		for(q = queues; q; q = q->next) {
			if(q->child) {
				for(c = q->child; c; c = c->next) {
					if(waitpid(c->pid, &s, WNOHANG) > 0) {
						rmchild(q, c->pid);
					}
				}
			}
			while((childlen(q) < q->maxchild) && queuelen(q) > 0) {
				addchild(q, mkchild(q));
			}
		}
		if(!poll(master)) {
		}
	}
	killpg(0, SIGHUP);
}

static void cleanuptcp(int fd, struct addrinfo *ai) {
	if (fd >= 0) {
		close(fd);
	}
	if (ai) {
		freeaddrinfo(ai);
	}
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
		cleanuptcp(fd, ai);
                exit(EXIT_FAILURE);
        }
        if ((fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0) {
                fprintf(stderr, "socket error: %s\n", gai_strerror(fd));
		cleanuptcp(fd, ai);
                exit(EXIT_FAILURE);
        }
        if ((status = bind(fd, ai->ai_addr, ai->ai_addrlen)) < 0) {
                fprintf(stderr, "bind error: %s\n", gai_strerror(fd));
		cleanuptcp(fd, ai);
                exit(EXIT_FAILURE);
        }
        if ((status = listen(fd, SOMAXCONN)) < 0) {
                fprintf(stderr, "listen error: %s\n", gai_strerror(fd));
		cleanuptcp(fd, ai);
                exit(EXIT_FAILURE);
        }

        freeaddrinfo(ai);
        return fd;
}

static void sighandler(int sig) {
	switch(sig) {
		case SIGINT:
		case SIGTERM:
			running = 0;
			close(masterfd);
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

static void cleanup(void) {
	job *j;
	queue *q;
	for(q = queues; q; q = q->next) {
		j = q->head;
		while(j) {
			j = j->next;
		}
		if(queuelen(q) == 0) {
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
		if(mkqueue(name, max, cmd) > 0) {
			fprintf(stderr, "queue `%s` already exists", name);
		}
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
				break;
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
	master(masterfd);
	close(masterfd);
	cleanup();
	fprintf(stderr, "exiting\n");
	exit(EXIT_SUCCESS);
}
