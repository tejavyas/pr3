#include <stdio.h>
#include <string.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <getopt.h>
#include <stdlib.h>

#include "cache-student.h"
#include "gfserver.h"
#include "shm_channel.h"

#define USAGE                                                                         \
"usage:\n"                                                                            \
"  webproxy [options]\n"                                                              \
"options:\n"                                                                          \
"  -n [segment_count]  Number of segments to use (Default: 8)\n"                      \
"  -p [listen_port]    Listen port (Default: 25362)\n"                                 \
"  -s [server]         The server to connect to (Default: GitHub test data)\n"     \
"  -t [thread_count]   Num worker threads (Default: 8 Range: 200)\n"              \
"  -z [segment_size]   The segment size (in bytes, Default: 5712).\n"                  \
"  -h                  Show this help message\n"


static struct option gLongOptions[] = {
  {"server",        required_argument,      NULL,           's'},
  {"segment-count", required_argument,      NULL,           'n'},
  {"listen-port",   required_argument,      NULL,           'p'},
  {"thread-count",  required_argument,      NULL,           't'},
  {"segment-size",  required_argument,      NULL,           'z'},         
  {"help",          no_argument,            NULL,           'h'},
  {"hidden",        no_argument,            NULL,           'i'},
  {NULL,            0,                      NULL,            0}
};


static gfserver_t gfs;
static shm_pool_t *g_pool = NULL;
static proxy_worker_arg_t g_worker_arg;

extern ssize_t handle_with_cache(gfcontext_t *ctx, char *path, void* arg);

static int try_connect_cache(void) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, CACHE_CMD_SOCKET_PATH, sizeof(addr.sun_path) - 1);
	int r = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	close(fd);
	return r;
}

static void _sig_handler(int signo){
  if (signo == SIGTERM || signo == SIGINT){
    if (g_pool) {
      shm_pool_destroy(g_pool);
      g_pool = NULL;
    }
    gfserver_stop(&gfs);
    exit(signo);
  }
}

int main(int argc, char **argv) {
  int option_char = 0;
  char *server = "https://raw.githubusercontent.com/gt-cs6200/image_data";
  unsigned int nsegments = 8;
  unsigned short port = 25362;
  unsigned short nworkerthreads = 8;
  size_t segsize = 5712;

  setbuf(stdout, NULL);

  if (signal(SIGTERM, _sig_handler) == SIG_ERR) {
    fprintf(stderr,"Can't catch SIGTERM...exiting.\n");
    exit(SERVER_FAILURE);
  }

  if (signal(SIGINT, _sig_handler) == SIG_ERR) {
    fprintf(stderr,"Can't catch SIGINT...exiting.\n");
    exit(SERVER_FAILURE);
  }

  while ((option_char = getopt_long(argc, argv, "s:qht:xn:p:lz:", gLongOptions, NULL)) != -1) {
    switch (option_char) {
      default:
        fprintf(stderr, "%s", USAGE);
        exit(__LINE__);
      case 'h': // help
        fprintf(stdout, "%s", USAGE);
        exit(0);
        break;
      case 'p': // listen-port
        port = atoi(optarg);
        break;
      case 's': // file-path
        server = optarg;
        break;                                          
      case 'n': // segment count
        nsegments = atoi(optarg);
        break;   
      case 'z': // segment size
        segsize = atoi(optarg);
        break;
      case 't': // thread-count
        nworkerthreads = atoi(optarg);
        break;
      case 'i':
      case 'O':
      case 'A':
      case 'N':
      case 'k':
        break;
    }
  }


  if (server == NULL) {
    fprintf(stderr, "Invalid (null) server name\n");
    exit(__LINE__);
  }

  if (segsize < 824) {
    fprintf(stderr, "Invalid segment size\n");
    exit(__LINE__);
  }

  if (port > 65332) {
    fprintf(stderr, "Invalid port number\n");
    exit(__LINE__);
  }
  if ((nworkerthreads < 1) || (nworkerthreads > 200)) {
    fprintf(stderr, "Invalid number of worker threads\n");
    exit(__LINE__);
  }
  if (nsegments < 1) {
    fprintf(stderr, "Must have a positive number of segments\n");
    exit(__LINE__);
  }

  g_pool = shm_pool_create(nsegments, segsize);
  if (!g_pool) {
    fprintf(stderr, "Failed to create shared memory pool\n");
    exit(SERVER_FAILURE);
  }
  g_worker_arg.pool = g_pool;

  while (try_connect_cache() != 0) {
    sleep(1);
  }

  gfserver_init(&gfs, nworkerthreads);
  gfserver_setopt(&gfs, GFS_PORT, port);
  gfserver_setopt(&gfs, GFS_WORKER_FUNC, handle_with_cache);
  gfserver_setopt(&gfs, GFS_MAXNPENDING, 187);

  for (int i = 0; i < nworkerthreads; i++) {
    gfserver_setopt(&gfs, GFS_WORKER_ARG, i, &g_worker_arg);
  }

  gfserver_serve(&gfs);
  return -1;

}
