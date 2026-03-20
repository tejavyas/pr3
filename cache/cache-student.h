#ifndef __CACHE_STUDENT_H__844
#define __CACHE_STUDENT_H__844

#include <stddef.h>
#include <stdint.h>

#define MAX_PATH_LEN 1024
#define SHM_NAME_MAX 64

typedef struct {
	char path[MAX_PATH_LEN];
	char shm_name[SHM_NAME_MAX];
	size_t seg_size;
} cache_request_t;

typedef struct {
	int found;
	size_t file_len;
} cache_response_t;

#define CHUNK_HEADER_SIZE 8
#define CACHE_RESPONSE_WIRE_SIZE (4 + 8)
#define CACHE_CMD_SOCKET_PATH "/tmp/pr3_cache_cmd_socket"

struct shm_pool;
typedef struct {
	struct shm_pool *pool;
} proxy_worker_arg_t;

#endif
