#ifndef SHM_CHANNEL_H
#define SHM_CHANNEL_H

#include <semaphore.h>
#include <stddef.h>
#include "cache-student.h"

typedef struct shm_pool shm_pool_t;

typedef struct {
	char *base;
	size_t seg_size;
	char shm_name[SHM_NAME_MAX];
	sem_t *sem_cw;
	sem_t *sem_pr;
	int shm_fd;
} shm_segment_t;

shm_pool_t *shm_pool_create(unsigned int nsegments, size_t segsize);
void shm_pool_destroy(shm_pool_t *pool);
int shm_pool_get(shm_pool_t *pool, shm_segment_t *out);
void shm_pool_release(shm_pool_t *pool, shm_segment_t *seg);
void shm_sem_names(const char *shm_name, char *sem_cw_out, char *sem_pr_out, size_t buf_size);
int shm_attach(const char *shm_name, size_t seg_size, void **ptr, int *fd_out, sem_t **sem_cw, sem_t **sem_pr);
void shm_detach(void *ptr, size_t seg_size, int fd, sem_t *sem_cw, sem_t *sem_pr);

#endif
