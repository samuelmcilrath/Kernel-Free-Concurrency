	#include <assert.h>
	#include <sys/types.h>
	#include <stdlib.h>
	#include "kfc.h"
	#include <ucontext.h>
	#include <pthread.h>
	#include "queue.h"
	#include <sys/ucontext.h> // For REG_RIP
    #include "valgrind.h"
	


	static int inited = 0;
	ucontext_t currC;

	ucontext_t scheduler; //context that acts as a scheduler
	kthread_t kthread_storage [];
	static kthread_mutex_t k_lock;
	static kthread_cond_t k_cond;

	static int id_count; //counter for current thread id
	tid_t curr_id; //t id for the running thread 

	static int k_count = 0;
	kthread_t k_ids[MAX_KTHREADS];

	tcb_t thread_storage [KFC_MAX_THREADS]; //pointer to global storage 
	static queue_t free_ids; //q of free spots from thread storage if running thread exits and gives up id
	queue_t thread_q; //fcfs q; this should be a queue of thread_id's not contexts


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

		//initialize scheduler 
		int err = getcontext(&scheduler);
		if(err == -1){ //check return 
			perror("getcontext err");
			return -1;
		}

		scheduler.uc_stack.ss_sp = malloc(KFC_DEF_STACK_SIZE);
        VALGRIND_STACK_REGISTER(scheduler.uc_stack.ss_sp, scheduler.uc_stack.ss_sp + KFC_DEF_STACK_SIZE);
		scheduler.uc_stack.ss_size = KFC_DEF_STACK_SIZE;
		scheduler.uc_stack.ss_flags = 0;
		//what to set link to?
		scheduler.uc_link = &thread_storage[0].context;
		makecontext(&scheduler, kfc_schedule, 0);

		
		/*initialize variables s*/
		
		id_count = 0; 
		curr_id =id_count++; //main thread gets id 0
		//main thread
		thread_storage[curr_id].id = curr_id;
		thread_storage[curr_id].fin = 0;
		thread_storage[curr_id].join_id = -1;
        thread_storage[curr_id].alloc = 0;
		//initialize id for later
		for(int i = 1; i < KFC_MAX_THREADS; i++)
			thread_storage[i].id = -1;
		
		//initialize q's 
		queue_init(&free_ids);
		queue_init(&thread_q);

		
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
	 */
	void
	kfc_teardown(void)
	{
		assert(inited);
		
		inited = 0;

		free(scheduler.uc_stack.ss_sp); //free scheduler
        for(int i = 0; i < KFC_MAX_THREADS; i++){
            if(thread_storage[i].alloc)
                free(thread_storage[i].context.uc_stack.ss_sp);
        }
		kthread_cond_destroy(&k_cond);
		kthread_mutex_destroy(&k_lock);
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
			thread_storage[*ptid].id = *ptid;
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
            thread_storage[*ptid].alloc = 1;		}
		else{
			thread_storage[*ptid].context.uc_stack.ss_sp = stack_base;
			thread_storage[*ptid].context.uc_stack.ss_size = stack_size;
            thread_storage[*ptid].alloc = 0;
		}
		
        VALGRIND_STACK_REGISTER(thread_storage[*ptid].context.uc_stack.ss_sp, thread_storage[*ptid].context.uc_stack.ss_sp + stack_size);
		thread_storage[*ptid].context.uc_stack.ss_flags = 0;
		thread_storage[*ptid].context.uc_link = &scheduler; //link should go to the scheduler
		thread_storage[*ptid].fin = 0;
		thread_storage[*ptid].join_id = -1;
		

		//make context
		makecontext(&thread_storage[*ptid].context, (void (*)())kfc_handle, 2, start_func, arg);

		assert(thread_storage[*ptid].context.uc_stack.ss_sp != NULL);	
		
		//add new thread to the q
		queue_enqueue(&thread_q, &thread_storage[*ptid].id);
		kthread_cond_signal(&k_cond);

		
		return 0;
	}

	/**
	 * Function to handle making the context and exiting threads 
	 */
	void 
	kfc_handle(void *(*start_func)(void *), void *arg){

		void *ret  = start_func(arg);
		kfc_exit(ret);
		abort(); //should't reach
		
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
		
		thread_storage[curr_id].ret = ret;
		thread_storage[curr_id].fin = 1;
		
		if(thread_storage[curr_id].join_id != -1)
			queue_insert_first(&thread_q, &thread_storage[thread_storage[curr_id].join_id].id); //put the joining thread up next
		swapcontext(&thread_storage[curr_id].context, &scheduler);

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
		
		getcontext(&thread_storage[curr_id].context);
		
		//if target thread hasn't finished, wait and send back to scheduler
		while(!thread_storage[tid].fin){
			thread_storage[tid].join_id = curr_id;
			swapcontext(&thread_storage[curr_id].context, &scheduler);
			
		}
		
		//Thread has finished and we want ret val
		if(pret != NULL){
			*pret = thread_storage[tid].ret;
		}
		
		if (thread_storage[tid].alloc) {
            free(thread_storage[tid].context.uc_stack.ss_sp);
            thread_storage[tid].alloc = 0; // Prevent double-free
        }
		
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

		//add current context to the q
		queue_enqueue(&thread_q, &thread_storage[curr_id].id);
		swapcontext(&thread_storage[curr_id].context, &scheduler);
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
		sem->count = value;
		queue_init(&sem->q);
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
		kthread_mutex_lock(&k_lock);
		assert(inited);
		sem->count++; //increment

		//if threads waiting 
		if(sem->q.size){
			queue_enqueue(&thread_q, queue_dequeue(&sem->q)); //inserts the thread waiting on the lock back to ready q
		}

		kthread_mutex_unlock(&k_lock);
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
		kthread_mutex_lock(&k_lock);
		assert(inited);

		//wait if no more resc left
		while(sem->count <= 0){
			queue_enqueue(&sem->q, &thread_storage[curr_id].id);
            kthread_mutex_unlock(&k_lock);
			swapcontext(&thread_storage[curr_id].context, &scheduler);
		}
		
		sem->count--;
		kthread_mutex_unlock(&k_lock);
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
		queue_destroy(&sem->q);
	}

	/**
	 * function to schedule next thread for fcfs
	 * should only be accessed by scheduler context
	 */
	void
	kfc_schedule(){

            //if thread isn't empty then get next item and swap into it
        //needs to be while loop for when we get back from swapcontext
        while (queue_size(&thread_q) > 0) {
            tid_t *next_id = (tid_t *) queue_dequeue(&thread_q);
            thread_storage[*next_id].context.uc_link = &scheduler; //make sure it returs to scheduler 
            curr_id = *next_id; //set the curr_id
            swapcontext(&scheduler, &thread_storage[*next_id].context); 
        }
	
	}