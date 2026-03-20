#include "gfserver.h"
#include "cache-student.h"
#include "shm_channel.h"
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>

static int connect_and_request(const char *path, const char *shm_name, size_t seg_size,
                               cache_response_t *resp) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, CACHE_CMD_SOCKET_PATH, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	cache_request_t req;
	memset(&req, 0, sizeof(req));
	strncpy(req.path, path, MAX_PATH_LEN - 1);
	req.path[MAX_PATH_LEN - 1] = '\0';
	strncpy(req.shm_name, shm_name, SHM_NAME_MAX - 1);
	req.shm_name[SHM_NAME_MAX - 1] = '\0';
	req.seg_size = seg_size;
	if (write(fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
		close(fd);
		return -1;
	}
	{
		unsigned char buf[CACHE_RESPONSE_WIRE_SIZE];
		if (read(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
			close(fd);
			return -1;
		}
		memcpy(&resp->found, buf, 4);
		uint64_t fl;
		memcpy(&fl, buf + 4, 8);
		resp->file_len = (size_t)fl;
	}
	close(fd);
	return 0;
}

ssize_t handle_with_cache(gfcontext_t *ctx, const char *path, void *arg) {
	proxy_worker_arg_t *wa = (proxy_worker_arg_t *)arg;
	if (!wa || !wa->pool) return SERVER_FAILURE;

	shm_segment_t seg;
	if (shm_pool_get(wa->pool, &seg) != 0) return SERVER_FAILURE;

	cache_response_t resp;
	if (connect_and_request(path, seg.shm_name, seg.seg_size, &resp) != 0) {
		shm_pool_release(wa->pool, &seg);
		return SERVER_FAILURE;
	}

	if (!resp.found) {
		shm_pool_release(wa->pool, &seg);
		if (gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0) < 0)
			return SERVER_FAILURE;
		return 0;
	}

	size_t file_len = resp.file_len;
	if (gfs_sendheader(ctx, GF_OK, file_len) < 0) {
		shm_pool_release(wa->pool, &seg);
		return SERVER_FAILURE;
	}

	size_t total_sent = 0;
	char *base = (char *)seg.base;
	while (total_sent < file_len) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += 2;
		if (sem_timedwait(seg.sem_pr, &ts) != 0) {
			shm_pool_release(wa->pool, &seg);
			return SERVER_FAILURE;
		}
		uint64_t chunk_u;
		memcpy(&chunk_u, base, CHUNK_HEADER_SIZE);
		size_t chunk_len = (size_t)chunk_u;
		if (chunk_len > seg.seg_size - CHUNK_HEADER_SIZE) {
			shm_pool_release(wa->pool, &seg);
			return SERVER_FAILURE;
		}
		ssize_t n = gfs_send(ctx, base + CHUNK_HEADER_SIZE, chunk_len);
		if (n < 0 || (size_t)n != chunk_len) {
			sem_post(seg.sem_cw);
			shm_pool_release(wa->pool, &seg);
			return SERVER_FAILURE;
		}
		total_sent += chunk_len;
		sem_post(seg.sem_cw);
	}

	shm_pool_release(wa->pool, &seg);
	if (total_sent != file_len)
		return SERVER_FAILURE;
	return (ssize_t)file_len;
}
