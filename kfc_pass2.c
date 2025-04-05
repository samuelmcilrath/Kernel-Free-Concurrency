#include <assert.h>
#include <sys/types.h>
#include <stdlib.h>
#include "kfc.h"
#include <ucontext.h>
#include "queue.h"
#include <sys/ucontext.h> // For REG_RIP


static int inited = 0;
ucontext_t currC;

ucontext_t scheduler; //context that acts as a scheduler
static int id_count; //counter for current thread id
tid_t curr_id; //t id for the running thread 
tcb_t thread_storage [KFC_MAX_THREADS]; //pointer to global storage 
static queue_t free_ids; //q of free spots from thread storage if running thread exits and gives up id
queue_t thread_q; //fcfs q

/**
 * Initializes the kfc library.  Programs are required to call this function
 * before they may use anything else in the library's public interface.
 *
 * @param kthreads    Number of kernel threads (pthreads) to allocate
 * @param quantum_us  Preemption timeslice in microseconds, or 0 for cooperative
 *                    scheduling
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_init(int kthreads, int quantum_us)
{
	assert(!inited);

	inited = 1;
	
	//initialize variables 
	id_count = 0; 
	curr_id =id_count++; //main thread gets id 0
	queue_init(&free_ids);
	queue_init(&free_ids);
	return 0;
}

/**
 * Cleans up any resources which were allocated by kfc_init.  You may assume
 * that this function is called only from the main thread, that any other
 * threads have terminated and been joined, and that threading will not be
 * needed again.  (In other words, just clean up and don't worry about the
 * consequences.)
 *
 * I won't be testing this function specifically, but it is provided as a
 * convenience to you if you are using Valgrind to check your code, which I
 * always encourage.
 * 
 * TODO
 * -destroy t storage
 * -destroy q 
 */
void
kfc_teardown(void)
{
	assert(inited);
	
	inited = 0;
}

/**
 * Creates a new user thread which executes the provided function concurrently.
 * It is left up to the implementation to decide whether the calling thread
 * continues to execute or the new thread takes over immediately.
 *
 *
 * @param ptid[out]   Pointer to a tid_t variable in which to store the new
 *                    thread's ID
 * @param start_func  Thread main function
 * @param arg         Argument to be passed to the thread main function
 * @param stack_base  Location of the thread's stack if already allocated, or
 *                    NULL if requesting that the library allocate it
 *                    dynamically
 * @param stack_size  Size (in bytes) of the thread's stack, or 0 to use the
 *                    default thread stack size KFC_DEF_STACK_SIZE
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_create(tid_t *ptid, void *(*start_func)(void *), void *arg,
		caddr_t stack_base, size_t stack_size)
{
	//grab addr of current tcb 
	getcontext(&thread_storage[curr_id].context); //set tcb of main thread 

	/*deal with thread id*/
	//check if there are free ids
	if(free_ids.size > 0){/*handle this later*/}
	else{
		*ptid = id_count++; 
	}
	
	
	
	int err = 0; //check err
	
	//first allocate new context 
	ucontext_t newC; 
	err = getcontext(&thread_storage[*ptid].context);
	if(err == -1){ //check return 
		perror("getcontext err");
		return -1;
	}

	//set to default if needed
	if(stack_size == 0)
		stack_size = KFC_DEF_STACK_SIZE;
	
	//setup newC stack
	if(stack_base == NULL){
		thread_storage[*ptid].context.uc_stack.ss_sp = malloc(stack_size);
		thread_storage[*ptid].context.uc_stack.ss_size = stack_size;
	}
	else{
		thread_storage[*ptid].context.uc_stack.ss_sp = stack_base;
		thread_storage[*ptid].context.uc_stack.ss_size = stack_size;
	}
	
	thread_storage[*ptid].context.uc_stack.ss_flags = 0;
	thread_storage[*ptid].context.uc_link = &thread_storage[curr_id].context; 
	
	//make context
	makecontext(&thread_storage[*ptid].context, (void (*)()) start_func, 1, arg);
	assert(thread_storage[*ptid].context.uc_stack.ss_sp != NULL);	
	

	//swap context
	int temp = curr_id;
	curr_id = *ptid; //change curr id

	err = swapcontext(&thread_storage[temp].context, &thread_storage[*ptid].context);	
	if(err == -1){
		perror("swap c");
		return -1;
	}

	curr_id = temp; //switch back curr id
	
	
	return 0;
}

/**
 * Exits the calling thread.  This should be the same thing that happens when
 * the thread's start_func returns.
 *
 * @param ret  Return value from the thread
 */
void
kfc_exit(void *ret)
{
	assert(inited);

	
}

/**
 * Waits for the thread specified by tid to terminate, retrieving that threads
 * return value.  Returns immediately if the target thread has already
 * terminated, otherwise blocks.  Attempting to join a thread which already has
 * another thread waiting to join it, or attempting to join a thread which has
 * already been joined, results in undefined behavior.
 *
 * @param pret[out]  Pointer to a void * in which the thread's return value from
 *                   kfc_exit should be stored, or NULL if the caller does not
 *                   care.
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_join(tid_t tid, void **pret)
{
	assert(inited);

	return 0;
}

/**
 * Returns a small integer which identifies the calling thread.
 *
 * @return Thread ID of the currently executing thread
 */
tid_t
kfc_self(void)
{
	assert(inited);

	return curr_id;
}

/**
 * Causes the calling thread to yield the processor voluntarily.  This may
 * result in another thread being scheduled, but it does not preclude the
 * possibility of the same thread continuing if re-chosen by the scheduling
 * algorithm.
 */
void
kfc_yield(void)
{
	assert(inited);
}

/**
 * Initializes a user-level counting semaphore with a specific value.
 *
 * @param sem    Pointer to the semaphore to be initialized
 * @param value  Initial value for the semaphore's counter
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_sem_init(kfc_sem_t *sem, int value)
{
	assert(inited);
	return 0;
}

/**
 * Increments the value of the semaphore.  This operation is also known as
 * up, signal, release, and V (Dutch verhoog, "increase").
 *
 * @param sem  Pointer to the semaphore which the thread is releasing
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_sem_post(kfc_sem_t *sem)
{
	assert(inited);
	return 0;
}

/**
 * Attempts to decrement the value of the semaphore.  This operation is also
 * known as down, acquire, and P (Dutch probeer, "try").  This operation should
 * block when the counter is not above 0.
 *
 * @param sem  Pointer to the semaphore which the thread wishes to acquire
 *
 * @return 0 if successful, nonzero on failure
 */
int
kfc_sem_wait(kfc_sem_t *sem)
{
	assert(inited);
	return 0;
}

/**
 * Frees any resources associated with a semaphore.  Destroying a semaphore on
 * which threads are waiting results in undefined behavior.
 *
 * @param sem  Pointer to the semaphore to be destroyed
 */
void
kfc_sem_destroy(kfc_sem_t *sem)
{
	assert(inited);
}
