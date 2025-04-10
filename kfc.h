#ifndef _KFC_H_
#define _KFC_H_

#include <stdio.h>
#include <sys/types.h>
#include <ucontext.h>
#include <pthread.h>
#include "kthread.h"
#include "queue.h"

#define DPRINTF(...) do { fprintf(stderr, __VA_ARGS__); } while (0)

#define KFC_MAX_THREADS 1024
#define KFC_DEF_STACK_SIZE 1048576

// Predefined thread IDs
#define KFC_TID_MAIN 0
#define KFC_TID_ERROR KFC_MAX_THREADS

// Thread identifier, which should be a small integer between
// 0 and KFC_MAX_THREADS - 1 (hint: array index)
typedef unsigned int tid_t;

typedef struct{
	tid_t id; //id field, should just be the index in the storage array
	int fin; //indicates if thread is finished
	int alloc;//indicates if stack was dynamically allocated
	int join_id; //id of thread trying to join

	ucontext_t context; 
	void *ret; //store return var

}tcb_t;

typedef struct {
	// Put fields for semaphore here
	int count; //curr num
	queue_t q;
} kfc_sem_t;

typedef struct{
	kthread_t k_id;
	tcb_t *curr_tcb;
	ucontext_t scheduler;
}ktcb_t;

/**************************
 * Public interface
 **************************/
int kfc_init(int kthreads, int quantum_us);
void kfc_teardown(void);
void *kfc_ktrampoline(void *arg);
int kfc_create(tid_t *ptid, void *(*start_func)(void *), void *arg,
		caddr_t stack_base, size_t stack_size);
void kfc_handle(void *(*start_func)(void *), void *arg);
void kfc_exit(void *ret);
int kfc_join(tid_t tid, void **pret);
tid_t kfc_self(void);
void kfc_yield(void);

int kfc_sem_init(kfc_sem_t *sem, int value);
int kfc_sem_post(kfc_sem_t *sem);
int kfc_sem_wait(kfc_sem_t *sem);
void kfc_sem_destroy(kfc_sem_t *sem);
void kfc_schedule();
int kfc_find_index();
#endif
