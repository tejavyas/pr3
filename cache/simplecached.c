#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <pthread.h>
#include "cache-student.h"
#include "shm_channel.h"
#include "simplecache.h"
#include "steque.h"
#include <stdint.h>

#if !defined(CACHE_FAILURE)
#define CACHE_FAILURE (-1)
#endif

#define MAX_CACHE_REQUEST_LEN 6112
#define MAX_SIMPLE_CACHE_QUEUE_SIZE 783

unsigned long int cache_delay;

static int g_listen_fd = -1;
static volatile int g_running = 1;
static steque_t g_queue;
static pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_queue_cond = PTHREAD_COND_INITIALIZER;
static int g_nthreads = 6;

static void _sig_handler(int signo) {
	if (signo == SIGTERM || signo == SIGINT) {
		g_running = 0;
		unlink(CACHE_CMD_SOCKET_PATH);
		if (g_listen_fd >= 0) {
			close(g_listen_fd);
			g_listen_fd = -1;
		}
		pthread_cond_broadcast(&g_queue_cond);
		exit(signo);
	}
}

#define USAGE \
"usage:\n" \
"  simplecached [options]\n" \
"options:\n" \
"  -c [cachedir]       Path to static files (Default: ./)\n" \
"  -t [thread_count]   Thread count for work queue (Default is 8, Range is 1-100)\n" \
"  -d [delay]          Delay in simplecache_get (Default is 0, Range is 0-2500000 (microseconds)\n " \
"  -h                  Show this help message\n"

static struct option gLongOptions[] = {
	{"cachedir", required_argument, NULL, 'c'},
	{"nthreads", required_argument, NULL, 't'},
	{"help", no_argument, NULL, 'h'},
	{"hidden", no_argument, NULL, 'i'},
	{"delay", required_argument, NULL, 'd'},
	{NULL, 0, NULL, 0}
};

void Usage(void) {
	fprintf(stdout, "%s", USAGE);
}

static void handle_client(int client_fd) {
	cache_request_t req;
	ssize_t n = read(client_fd, &req, sizeof(req));
	if (n != (ssize_t)sizeof(req)) {
		close(client_fd);
		return;
	}

	int fd = simplecache_get(req.path);
	if (fd < 0) {
		unsigned char buf[CACHE_RESPONSE_WIRE_SIZE];
		memset(buf, 0, sizeof(buf));
		int zero = 0;
		memcpy(buf, &zero, 4);
		if (write(client_fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
			close(client_fd);
			return;
		}
		close(client_fd);
		return;
	}

	struct stat st;
	if (fstat(fd, &st) != 0) {
		unsigned char buf[CACHE_RESPONSE_WIRE_SIZE];
		memset(buf, 0, sizeof(buf));
		int zero = 0;
		memcpy(buf, &zero, 4);
		write(client_fd, buf, sizeof(buf));
		close(fd);
		close(client_fd);
		return;
	}
	size_t file_len = (size_t)st.st_size;
	{
		unsigned char buf[CACHE_RESPONSE_WIRE_SIZE];
		int one = 1;
		memcpy(buf, &one, 4);
		uint64_t fl = (uint64_t)file_len;
		memcpy(buf + 4, &fl, 8);
		if (write(client_fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
			close(fd);
			close(client_fd);
			return;
		}
	}

	void *shm_ptr = NULL;
	int shm_fd = -1;
	sem_t *sem_cw = NULL, *sem_pr = NULL;
	if (shm_attach(req.shm_name, req.seg_size, &shm_ptr, &shm_fd, &sem_cw, &sem_pr) != 0) {
		close(fd);
		close(client_fd);
		return;
	}

	size_t data_cap = req.seg_size - CHUNK_HEADER_SIZE;
	char *header_slot = (char *)shm_ptr - CHUNK_HEADER_SIZE;
	size_t remaining = file_len;

	while (remaining > 0) {
		if (sem_wait(sem_cw) != 0) break;
		size_t chunk = remaining > data_cap ? data_cap : remaining;
		uint64_t chunk_u = (uint64_t)chunk;
		memcpy(header_slot, &chunk_u, CHUNK_HEADER_SIZE);
		ssize_t rd = read(fd, shm_ptr, chunk);
		if (rd <= 0 || (size_t)rd != chunk) break;
		remaining -= chunk;
		sem_post(sem_pr);
	}

	close(fd);
	shm_detach(shm_ptr, req.seg_size, shm_fd, sem_cw, sem_pr);
	close(client_fd);
}

static void *worker_thread(void *arg) {
	(void)arg;
	while (g_running) {
		pthread_mutex_lock(&g_queue_mutex);
		while (g_running && steque_isempty(&g_queue))
			pthread_cond_wait(&g_queue_cond, &g_queue_mutex);
		if (!g_running) {
			pthread_mutex_unlock(&g_queue_mutex);
			break;
		}
		int *pfd = (int *)steque_pop(&g_queue);
		pthread_mutex_unlock(&g_queue_mutex);
		if (!pfd) continue;
		int client_fd = *pfd;
		free(pfd);
		handle_client(client_fd);
	}
	return NULL;
}

int main(int argc, char **argv) {
	int nthreads = 6;
	char *cachedir = "locals.txt";
	int option_char;

	setbuf(stdout, NULL);

	while ((option_char = getopt_long(argc, argv, "d:ic:hlt:x", gLongOptions, NULL)) != -1) {
		switch (option_char) {
			default:
				Usage();
				exit(1);
			case 't':
				nthreads = atoi(optarg);
				break;
			case 'h':
				Usage();
				exit(0);
			case 'c':
				cachedir = optarg;
				break;
			case 'd':
				cache_delay = (unsigned long int)atoi(optarg);
				break;
			case 'i':
			case 'o':
			case 'a':
				break;
		}
	}

	if (cache_delay > 2500000) {
		fprintf(stderr, "Cache delay must be less than 2500000 (us)\n");
		exit(__LINE__);
	}
	if (nthreads < 1 || nthreads > 100) {
		fprintf(stderr, "Invalid number of threads must be in between 1-100\n");
		exit(__LINE__);
	}
	g_nthreads = nthreads;

	if (signal(SIGINT, _sig_handler) == SIG_ERR) {
		fprintf(stderr, "Unable to catch SIGINT...exiting.\n");
		exit(CACHE_FAILURE);
	}
	if (signal(SIGTERM, _sig_handler) == SIG_ERR) {
		fprintf(stderr, "Unable to catch SIGTERM...exiting.\n");
		exit(CACHE_FAILURE);
	}

	simplecache_init(cachedir);
	steque_init(&g_queue);

	unlink(CACHE_CMD_SOCKET_PATH);
	g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (g_listen_fd < 0) {
		perror("socket");
		exit(CACHE_FAILURE);
	}
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, CACHE_CMD_SOCKET_PATH, sizeof(addr.sun_path) - 1);
	if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("bind");
		close(g_listen_fd);
		exit(CACHE_FAILURE);
	}
	if (listen(g_listen_fd, 128) != 0) {
		perror("listen");
		close(g_listen_fd);
		unlink(CACHE_CMD_SOCKET_PATH);
		exit(CACHE_FAILURE);
	}

	pthread_t *threads = malloc((size_t)nthreads * sizeof(pthread_t));
	if (!threads) {
		close(g_listen_fd);
		unlink(CACHE_CMD_SOCKET_PATH);
		exit(CACHE_FAILURE);
	}
	for (int i = 0; i < nthreads; i++) {
		if (pthread_create(&threads[i], NULL, worker_thread, NULL) != 0) {
			g_running = 0;
			for (int j = 0; j < i; j++) pthread_join(threads[j], NULL);
			free(threads);
			close(g_listen_fd);
			unlink(CACHE_CMD_SOCKET_PATH);
			exit(CACHE_FAILURE);
		}
	}

	while (g_running) {
		int client_fd = accept(g_listen_fd, NULL, NULL);
		if (client_fd < 0) {
			if (!g_running) break;
			continue;
		}
		int *pfd = malloc(sizeof(int));
		if (!pfd) {
			close(client_fd);
			continue;
		}
		*pfd = client_fd;
		pthread_mutex_lock(&g_queue_mutex);
		steque_enqueue(&g_queue, pfd);
		pthread_cond_signal(&g_queue_cond);
		pthread_mutex_unlock(&g_queue_mutex);
	}

	close(g_listen_fd);
	g_listen_fd = -1;
	unlink(CACHE_CMD_SOCKET_PATH);

	for (int i = 0; i < nthreads; i++)
		pthread_join(threads[i], NULL);
	free(threads);
	steque_destroy(&g_queue);
	return -1;
}
