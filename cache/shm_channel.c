#define _POSIX_C_SOURCE 200809L
#include "shm_channel.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

#define PREFIX_SHM "/pr3_shm_"
#define PREFIX_CW  "/pr3_cw_"
#define PREFIX_PR  "/pr3_pr_"

struct shm_pool {
	unsigned int nsegments;
	size_t segsize;
	shm_segment_t *segments;
	unsigned int *free_stack;
	unsigned int free_top;
	pthread_mutex_t lock;
	pthread_cond_t cond;
};

static void sem_names_from_shm(const char *shm_name, char *sem_cw, char *sem_pr) {
	const char *base = strstr(shm_name, "shm_");
	if (base)
		base += 4;
	else
		base = shm_name;
	snprintf(sem_cw, SHM_NAME_MAX, "%s%s", PREFIX_CW, base);
	snprintf(sem_pr, SHM_NAME_MAX, "%s%s", PREFIX_PR, base);
}

void shm_sem_names(const char *shm_name, char *sem_cw_out, char *sem_pr_out, size_t buf_size) {
	char cw[SHM_NAME_MAX], pr[SHM_NAME_MAX];
	sem_names_from_shm(shm_name, cw, pr);
	strncpy(sem_cw_out, cw, buf_size - 1);
	sem_cw_out[buf_size - 1] = '\0';
	strncpy(sem_pr_out, pr, buf_size - 1);
	sem_pr_out[buf_size - 1] = '\0';
}

shm_pool_t *shm_pool_create(unsigned int nsegments, size_t segsize) {
	unsigned int i;
	shm_pool_t *pool = calloc(1, sizeof(shm_pool_t));
	if (!pool) return NULL;
	pool->nsegments = nsegments;
	pool->segsize = segsize;
	pool->segments = calloc(nsegments, sizeof(shm_segment_t));
	pool->free_stack = malloc(nsegments * sizeof(unsigned int));
	if (!pool->segments || !pool->free_stack) {
		free(pool->segments);
		free(pool->free_stack);
		free(pool);
		return NULL;
	}
	pthread_mutex_init(&pool->lock, NULL);
	pthread_cond_init(&pool->cond, NULL);

	pid_t pid = getpid();
	for (i = 0; i < nsegments; i++) {
		shm_segment_t *s = &pool->segments[i];
		snprintf(s->shm_name, SHM_NAME_MAX, "%s%d_%u", PREFIX_SHM, (int)pid, i);
		s->seg_size = segsize;
		s->shm_fd = shm_open(s->shm_name, O_CREAT | O_RDWR | O_EXCL, 0600);
		if (s->shm_fd < 0) {
			while (i > 0) {
				--i;
				munmap(pool->segments[i].base, pool->segsize);
				close(pool->segments[i].shm_fd);
				sem_close(pool->segments[i].sem_cw);
				sem_close(pool->segments[i].sem_pr);
				shm_unlink(pool->segments[i].shm_name);
			}
			pthread_cond_destroy(&pool->cond);
			pthread_mutex_destroy(&pool->lock);
			free(pool->segments);
			free(pool->free_stack);
			free(pool);
			return NULL;
		}
		if (ftruncate(s->shm_fd, (off_t)segsize) < 0) {
			close(s->shm_fd);
			shm_unlink(s->shm_name);
			goto cleanup;
		}
		s->base = mmap(NULL, segsize, PROT_READ | PROT_WRITE, MAP_SHARED, s->shm_fd, 0);
		if (s->base == MAP_FAILED) {
			close(s->shm_fd);
			shm_unlink(s->shm_name);
			goto cleanup;
		}
		char sem_cw_name[SHM_NAME_MAX], sem_pr_name[SHM_NAME_MAX];
		sem_names_from_shm(s->shm_name, sem_cw_name, sem_pr_name);
		sem_unlink(sem_cw_name);
		sem_unlink(sem_pr_name);
		s->sem_cw = sem_open(sem_cw_name, O_CREAT | O_EXCL, 0600, 0);
		s->sem_pr = sem_open(sem_pr_name, O_CREAT | O_EXCL, 0600, 0);
		if (s->sem_cw == SEM_FAILED || s->sem_pr == SEM_FAILED) {
			if (s->sem_cw != SEM_FAILED) sem_close(s->sem_cw);
			if (s->sem_pr != SEM_FAILED) sem_close(s->sem_pr);
			munmap(s->base, segsize);
			close(s->shm_fd);
			shm_unlink(s->shm_name);
			goto cleanup;
		}
		sem_post(s->sem_cw);
		pool->free_stack[i] = i;
	}
	pool->free_top = nsegments;
	return pool;
cleanup:
	while (i > 0) {
		--i;
		shm_segment_t *t = &pool->segments[i];
		munmap(t->base, pool->segsize);
		close(t->shm_fd);
		sem_close(t->sem_cw);
		sem_close(t->sem_pr);
		shm_unlink(t->shm_name);
	}
	pthread_cond_destroy(&pool->cond);
	pthread_mutex_destroy(&pool->lock);
	free(pool->segments);
	free(pool->free_stack);
	free(pool);
	return NULL;
}

void shm_pool_destroy(shm_pool_t *pool) {
	if (!pool) return;
	for (unsigned int i = 0; i < pool->nsegments; i++) {
		shm_segment_t *s = &pool->segments[i];
		munmap(s->base, pool->segsize);
		close(s->shm_fd);
		sem_close(s->sem_cw);
		sem_close(s->sem_pr);
		shm_unlink(s->shm_name);
		char sem_cw_name[SHM_NAME_MAX], sem_pr_name[SHM_NAME_MAX];
		sem_names_from_shm(s->shm_name, sem_cw_name, sem_pr_name);
		sem_unlink(sem_cw_name);
		sem_unlink(sem_pr_name);
	}
	pthread_cond_destroy(&pool->cond);
	pthread_mutex_destroy(&pool->lock);
	free(pool->segments);
	free(pool->free_stack);
	free(pool);
}

int shm_pool_get(shm_pool_t *pool, shm_segment_t *out) {
	pthread_mutex_lock(&pool->lock);
	while (pool->free_top == 0)
		pthread_cond_wait(&pool->cond, &pool->lock);
	unsigned int idx = pool->free_stack[--pool->free_top];
	pthread_mutex_unlock(&pool->lock);
	*out = pool->segments[idx];
	out->seg_size = pool->segsize;
	return 0;
}

void shm_pool_release(shm_pool_t *pool, shm_segment_t *seg) {
	while (sem_trywait(seg->sem_cw) == 0);
	while (sem_trywait(seg->sem_pr) == 0);
	sem_post(seg->sem_cw);

	pthread_mutex_lock(&pool->lock);
	for (unsigned int i = 0; i < pool->nsegments; i++) {
		if (pool->segments[i].base == seg->base) {
			pool->free_stack[pool->free_top++] = i;
			break;
		}
	}
	pthread_cond_signal(&pool->cond);
	pthread_mutex_unlock(&pool->lock);
}

int shm_attach(const char *shm_name, size_t seg_size, void **ptr, int *fd_out, sem_t **sem_cw, sem_t **sem_pr) {
	int fd = shm_open(shm_name, O_RDWR, 0);
	if (fd < 0) return -1;
	void *p = mmap(NULL, seg_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		close(fd);
		return -1;
	}
	char sem_cw_name[SHM_NAME_MAX], sem_pr_name[SHM_NAME_MAX];
	sem_names_from_shm(shm_name, sem_cw_name, sem_pr_name);
	sem_t *cw = sem_open(sem_cw_name, 0);
	sem_t *pr = sem_open(sem_pr_name, 0);
	if (cw == SEM_FAILED || pr == SEM_FAILED) {
		if (cw != SEM_FAILED) sem_close(cw);
		if (pr != SEM_FAILED) sem_close(pr);
		munmap(p, seg_size);
		close(fd);
		return -1;
	}
	*ptr = (char *)p + CHUNK_HEADER_SIZE;
	*fd_out = fd;
	*sem_cw = cw;
	*sem_pr = pr;
	return 0;
}

void shm_detach(void *ptr, size_t seg_size, int fd, sem_t *sem_cw, sem_t *sem_pr) {
	void *base = (char *)ptr - CHUNK_HEADER_SIZE;
	munmap(base, seg_size);
	close(fd);
	sem_close(sem_cw);
	sem_close(sem_pr);
}
