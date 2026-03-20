#include "proxy-student.h"
#include "gfserver.h"
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#define BUFSIZE 8192
#define INITIAL_BUF_CAP 65536

typedef struct {
	char *data;
	size_t capacity;
	size_t size;
} buffer_t;

static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
	buffer_t *buf = (buffer_t *) userdata;
	size_t total = size * nmemb;
	if (buf->size + total > buf->capacity) {
		size_t new_cap = buf->capacity ? buf->capacity * 2 : INITIAL_BUF_CAP;
		while (new_cap < buf->size + total)
			new_cap *= 2;
		char *new_data = realloc(buf->data, new_cap);
		if (!new_data)
			return 0;
		buf->data = new_data;
		buf->capacity = new_cap;
	}
	memcpy(buf->data + buf->size, ptr, total);
	buf->size += total;
	return total;
}

ssize_t handle_with_curl(gfcontext_t *ctx, const char *path, void *arg) {
	const char *base_server = (const char *) arg;
	if (!base_server || !path) {
		errno = EINVAL;
		return SERVER_FAILURE;
	}

	size_t base_len = strlen(base_server);
	size_t path_len = strlen(path);
	int need_slash = (base_len > 0 && base_server[base_len - 1] != '/' && path[0] != '/');
	size_t url_len = base_len + path_len + (need_slash ? 2 : 1);
	char *url = malloc(url_len);
	if (!url) {
		errno = ENOMEM;
		return SERVER_FAILURE;
	}
	memcpy(url, base_server, base_len);
	if (need_slash)
		url[base_len] = '/';
	memcpy(url + base_len + (need_slash ? 1 : 0), path, path_len + 1);

	buffer_t buf = { .data = NULL, .capacity = 0, .size = 0 };

	CURL *curl = curl_easy_init();
	if (!curl) {
		free(url);
		errno = ENOMEM;
		return SERVER_FAILURE;
	}

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

	CURLcode res = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	curl_easy_cleanup(curl);
	free(url);

	if (res != CURLE_OK) {
		free(buf.data);
		errno = EIO;
		return SERVER_FAILURE;
	}

	if (http_code == 404 || http_code == 403) {
		free(buf.data);
		if (gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0) < 0)
			return SERVER_FAILURE;
		return 0;
	}

	size_t file_len = buf.size;
	if (gfs_sendheader(ctx, GF_OK, file_len) < 0) {
		free(buf.data);
		return SERVER_FAILURE;
	}

	size_t sent = 0;
	while (sent < file_len) {
		size_t chunk = file_len - sent;
		if (chunk > BUFSIZE)
			chunk = BUFSIZE;
		ssize_t n = gfs_send(ctx, buf.data + sent, chunk);
		if (n < 0 || (size_t) n != chunk) {
			free(buf.data);
			return SERVER_FAILURE;
		}
		sent += (size_t) n;
	}

	free(buf.data);
	return (ssize_t) file_len;
}

ssize_t handle_with_file(gfcontext_t *ctx, const char *path, void *arg) {
	return handle_with_curl(ctx, path, arg);
}
