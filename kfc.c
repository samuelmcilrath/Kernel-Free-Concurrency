	#include <assert.h>
	#include <sys/types.h>
	#include <stdlib.h>
	#include "kfc.h"
	#include <ucontext.h>
	#include <pthread.h>
	#include "queue.h"
	#include <sys/ucontext.h> // For REG_RIP
	#include "valgrind/valgrind.h"

	
	//TODO need to make some sort of kernel storage 
	//right now all the kernel threads are sharing the same scheduler context; 

	static int inited = 0;

	static int id_count; //counter for current thread id
	
	tcb_t thread_storage [KFC_MAX_THREADS]; //pointer to global storage 
	queue_t thread_q; //fcfs q; this should be a queue of thread_id's not contexts

	//m2m
	kthread_mutex_t k_lock;
	kthread_mutex_t q_lock;
	kthread_mutex_t sem_lock;
	kthread_mutex_t t_lock; //this is for thread storage and swap contexts
	kthread_cond_t q_cond;
	ktcb_t k_storage[MAX_KTHREADS];
	int num_kthreads;

	int shutdown = 0;

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
		num_kthreads = kthreads;
		inited = 1;

		//initialize main kthread scheduler
		k_storage[0].k_id = kthread_self();
		getcontext(&k_storage[0].scheduler);
		k_storage[0].scheduler.uc_stack.ss_sp = malloc(KFC_DEF_STACK_SIZE);
		k_storage[0].scheduler.uc_stack.ss_size = KFC_DEF_STACK_SIZE;
		k_storage[0].scheduler.uc_stack.ss_flags = 0;
        k_storage[0].scheduler.uc_link = NULL;
		makecontext(&k_storage[0].scheduler, kfc_schedule, 0);

		/*initialize variables s*/
		id_count = 0; 
		k_storage[0].curr_tcb = &thread_storage[id_count++]; //main thread gets id 0

		//main thread
		thread_storage[0].id = 0;
		thread_storage[0].fin = 0;
		thread_storage[0].join_id = -1;
		thread_storage[0].alloc = 0;
		getcontext(&thread_storage[0].context);

		//initialize thread_ids
		for(int i = 1; i < KFC_MAX_THREADS; i++)
			thread_storage[i].id = -1;
		
		//initialize q's 
		queue_init(&thread_q);
		
		kthread_mutex_init(&k_lock);
		kthread_mutex_init(&q_lock);
		kthread_mutex_init(&sem_lock);
		kthread_cond_init(&q_cond);
		
		//initialize kthreads
		if(kthreads > 1){
			for(int i = 1; i < kthreads ; i++){
                int *arg = malloc(sizeof(int));
                *arg = i;
				kthread_create(&k_storage[i].k_id, kfc_ktrampoline, arg);
			}
		}
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

		//free(scheduler.uc_stack.ss_sp); //free scheduler

		shutdown = 1;

		//signal any waiting threads 
		for(int i = 0; i < num_kthreads; i++)
			kthread_cond_broadcast(&q_cond);
		
		kthread_cond_destroy(&q_cond);
		kthread_mutex_destroy(&k_lock);
	}

	/**
	 * essentially does what init does but for other kthreads
	 * needed in order to properly get kthread_self value
	 * 
	 * @param index  index into the k_storage to store kthread info
	 * maybe change return val to 0?
	 */
	void *	
	kfc_ktrampoline(void *arg){
		
		int index = *(int *)arg;
		DPRINTF("TRAMPOLINE - kself:%d kindex:%d\n", kthread_self(), index);	

		assert(k_storage[index].k_id == kthread_self()); //should be = because kthread_create

		k_storage[index].k_id = kthread_self(); 

		//scheduler setup
		getcontext(&k_storage[index].scheduler);

		k_storage[index].scheduler.uc_stack.ss_sp = malloc(KFC_DEF_STACK_SIZE);
		k_storage[index].scheduler.uc_stack.ss_size = KFC_DEF_STACK_SIZE;
		k_storage[index].scheduler.uc_stack.ss_flags = 0;
		VALGRIND_STACK_REGISTER(k_storage[index].scheduler.uc_stack.ss_sp, k_storage[index].scheduler.uc_stack.ss_sp + KFC_DEF_STACK_SIZE);
		
		makecontext(&k_storage[index].scheduler, kfc_schedule, 0);
		kthread_mutex_lock(&q_lock);
		DPRINTF("tramp - Q locked !!!\n");
		setcontext(&k_storage[index].scheduler);

		return NULL;
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
		
		int index = kfc_find_index();

		/*deal with thread id*/
        kthread_mutex_lock(&k_lock);

        *ptid = id_count++; 
        thread_storage[*ptid].id = *ptid;

        kthread_mutex_unlock(&k_lock);
		
		DPRINTF("kself:%d - CREATE -  kindex:%d ptid:%d\n", kthread_self(), index, *ptid);
		
		
		//first allocate new context 
		int err = 0; //check err
		err = getcontext(&thread_storage[*ptid].context);
		if(err == -1){ //check return 
			perror("getcontext err");
			return -1;
		}

		//set to default if needed
		if(stack_size == 0)
			stack_size = KFC_DEF_STACK_SIZE;
		
		//setup new stack
		if(stack_base == NULL){
			thread_storage[*ptid].context.uc_stack.ss_sp = malloc(stack_size);
			thread_storage[*ptid].context.uc_stack.ss_size = stack_size;
			thread_storage[*ptid].alloc = 1;
		}
		else{
			thread_storage[*ptid].context.uc_stack.ss_sp = stack_base;
			thread_storage[*ptid].context.uc_stack.ss_size = stack_size;
			thread_storage[*ptid].alloc = 0;
		}
		
		VALGRIND_STACK_REGISTER(thread_storage[*ptid].context.uc_stack.ss_sp, thread_storage[*ptid].context.uc_stack.ss_sp + stack_size);
		thread_storage[*ptid].context.uc_stack.ss_flags = 0;
		thread_storage[*ptid].context.uc_link = &k_storage[index].scheduler; //link should go to the scheduler
		thread_storage[*ptid].fin = 0;
		thread_storage[*ptid].join_id = -1;
		

		//make context
		makecontext(&thread_storage[*ptid].context, (void (*)())kfc_handle, 2, start_func, arg);

		assert(thread_storage[*ptid].context.uc_stack.ss_sp != NULL);	
		
		//add new thread to the q
		kthread_mutex_lock(&q_lock);

		DPRINTF("create - Q locked !!!\n");
		DPRINTF("enq @create id %d", thread_storage[*ptid].id);

		queue_enqueue(&thread_q, &thread_storage[*ptid]);
		kthread_cond_broadcast(&q_cond); //??try to move outside later??

		kthread_mutex_unlock(&q_lock);
		DPRINTF("create - Q Unlocked !!!\n");
		
		return 0;
	}

	/**
	 * Function to handle making the context and exiting threads 
	 */
	void 
	kfc_handle(void *(*start_func)(void *), void *arg){

		DPRINTF("kself:%d - HANDLE -\n", kthread_self());

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
		
		int index = kfc_find_index();

		DPRINTF("kself:%d - EXIT -  kindex:%d pid:%d\n" , kthread_self(), index, k_storage[index].curr_tcb->id);
		assert(inited);
		
        //set TCB fields
		k_storage[index].curr_tcb->ret = ret;
		k_storage[index].curr_tcb->fin = 1;
		
        //If thread is waiting to join -> put waiting thread back on ready q
		if(k_storage[index].curr_tcb->join_id != -1){
			kthread_mutex_lock(&q_lock);

			DPRINTF("exit - Q locked !!!\n");
			DPRINTF("Enq @exit join id %d\n",k_storage[index].curr_tcb->join_id);

			queue_enqueue(&thread_q, &thread_storage[k_storage[index].curr_tcb->join_id]);
			//queue_enqueue(&thread_q, &thread_storage[thread_storage[k_storage[index].curr_id].join_id].id); //put the joining thread up next
			kthread_cond_broadcast(&q_cond);
			kthread_mutex_unlock(&q_lock);
			DPRINTF("exit - Q Unlocked !!!\n");
		}
		kthread_mutex_lock(&q_lock); //this was commented out but I think was problematic

		DPRINTF("exit bot - Q locked !!!\n");

		setcontext(&k_storage[index].scheduler); //pretty sure this needs to be here

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
		

		int index = kfc_find_index();
		DPRINTF("kself:%d - JOIN - kindex:%d\n", kthread_self(), index);		

		assert(inited);
		DPRINTF("curr : %d joining with tid: %d fin status - %d \n", k_storage[index].curr_tcb->id, tid, thread_storage[tid].fin);
		
		//if target thread hasn't finished, wait and send back to scheduler
		if(!thread_storage[tid].fin){
			thread_storage[tid].join_id = k_storage[index].curr_tcb->id;
			kthread_mutex_lock(&q_lock);
			DPRINTF("join - Q locked !!!\n");
			swapcontext(&k_storage[index].curr_tcb->context, &k_storage[index].scheduler);
		}
		
		
		//Thread has finished and we want ret val
		if(pret != NULL){
			*pret = thread_storage[tid].ret;
		}
		
		//free(thread_storage[tid].context.uc_stack.ss_sp); //free the joined thread
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
		return k_storage[kfc_find_index()].curr_tcb->id;
		
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
		
		int index = kfc_find_index();
		DPRINTF("kself:%d - YIELD -  kindex:%d\n", kthread_self(), index);		

		assert(inited);

		//add current context to the q
		kthread_mutex_lock(&q_lock);
		DPRINTF("yield - Q locked !!!\n");
		DPRINTF("enq @yield id %d\n", k_storage[index].curr_tcb->id);
		queue_enqueue(&thread_q, &k_storage[index].curr_tcb);
		//queue_enqueue(&thread_q, &thread_storage[k_storage[index].curr_id].id);
		kthread_cond_broadcast(&q_cond);
		//kthread_mutex_unlock(&q_lock);
		
		swapcontext(&k_storage[index].curr_tcb->context, &k_storage[index].scheduler);
		
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
		DPRINTF("kself:%d - SEMINIT - \n", kthread_self());		

		kthread_mutex_lock(&sem_lock); //*may not need*
		assert(inited);
		sem->count = value;
		queue_init(&sem->q);
		kthread_mutex_unlock(&sem_lock);
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
		DPRINTF("kself:%d - SEM POST -\n", kthread_self());		

		kthread_mutex_lock(&sem_lock);
		assert(inited);
		sem->count++; //increment

		//if threads waiting 
		if(sem->q.size){
			kthread_mutex_lock(&q_lock);
			DPRINTF("sempost - Q locked !!!\n");
			DPRINTF("Enq @sempost\n");
			queue_enqueue(&thread_q, queue_dequeue(&sem->q)); //inserts the thread waiting on the lock back to ready q
			kthread_cond_broadcast(&q_cond);
			kthread_mutex_unlock(&q_lock);
			DPRINTF("\nsem- Q Unlocked !!!\n");
		}
		
		kthread_mutex_unlock(&sem_lock);
		DPRINTF("\nsem - Q Unlocked !!!\n");
		return 0;
	}

	/**
	 * Attempts to decrement the value of the semaphore.  This operation is also
	 * known as down, acquire, and P (Dutch probeer, "try").  This operation should
	 * block when the counter is not above 0.
	 *
	 * @param sem  Pointer to the semaphore which the thread wishes to acquire
	 * .
	 * @return 0 if successful, nonzero on failure
	 */
	int
	kfc_sem_wait(kfc_sem_t *sem)
	{
		
		int index = kfc_find_index();
		DPRINTF(" kself:%d - SEMWAIT - kindex:%d\n", kthread_self(), index);		

		assert(inited);
		//DPRINTF("id %d calling wait\n", curr_id);
		
		//wait if no more resc left
		kthread_mutex_lock(&sem_lock);
		while(sem->count <= 0){
			//getcontext(&thread_storage[curr_id].context); //save the context
			queue_enqueue(&sem->q, &k_storage[index].curr_tcb);
			//queue_enqueue(&sem->q, &thread_storage[k_storage[index].curr_id].id);
			
			kthread_mutex_unlock(&sem_lock);
			
			kthread_mutex_lock(&q_lock);
			DPRINTF("\nsem wait - Q locked !!!\n");
			swapcontext(&k_storage[index].curr_tcb->context, &k_storage[index].scheduler);
			kthread_mutex_lock(&sem_lock);
		
		}
		sem->count--;
		kthread_mutex_unlock(&sem_lock);
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
		DPRINTF("kself:%d - SEMDESTROY - \n", kthread_self());		

		queue_destroy(&sem->q);
	}

	/**
	 * function to schedule next thread for fcfs
	 * should only be accessed by scheduler context
	 */
	void
	kfc_schedule(){

		DPRINTF(" kself:%d - SCHEUDLE (outside unlock) -\n", kthread_self());	

		kthread_mutex_unlock(&q_lock);

		DPRINTF("\nsched - Q Unlocked !!!\n");

		
				
		//when the queue doesn't have anything, block until signalled
		int index = kfc_find_index();
		kthread_mutex_lock(&q_lock);

		DPRINTF("schedule top - Q locked !!!\n");
		DPRINTF("kself:%d - SCHEDULE (inside unlock) -  kindex:%d\n", kthread_self(), index);

		while(queue_size(&thread_q) == 0){
			if(shutdown){
				kthread_mutex_unlock(&q_lock);
				DPRINTF("sched shutdown - Q Unlocked !!!\n");
				return;
			}
			DPRINTF("kself:%d SCHEDULE (cond wait) - kindex:%d\n", kthread_self(), index);

			kthread_cond_wait(&q_cond, &q_lock); //we wait until signaled 

			DPRINTF("kself:%d - SCHEDULE (cond **WAKEUP**) - kindex:%d\n", kthread_self(), index);

		}
			
		DPRINTF("kself:%d - SCHEDULE (past cond wait) -kindex:%d\n", kthread_self(), index);
		//if thread isn't empty then get next item and swap into it
		//needs to be while loop for when we get back from swapcontext
		
		
		assert(queue_size(&thread_q) != 0); //sanity check (this has failed in the past)
		k_storage[index].curr_tcb = (tcb_t *) queue_dequeue(&thread_q);		
		kthread_cond_broadcast(&q_cond);
		kthread_mutex_unlock(&q_lock);
		DPRINTF("sched(bot) - Q Unlocked !!!\n");
		
		setcontext(&k_storage[index].curr_tcb->context); //how do we ensure this isn't a stale context?

		//should never make it past here
		DPRINTF("**SHOULD NEVER MAKE IT HERE (Scheduler)** \n");
	}

	/**
	 * search through k_storage and find index corresponding to kthread_self
	 * @return indexd on success and -1 on failure
	 */
	int
	kfc_find_index(){
		DPRINTF("kself:%d - FINDINDEX\n", kthread_self());		

		int id = kthread_self();
		for(int i = 0; i < num_kthreads; i++)
			if(k_storage[i].k_id == id)
				return i;
		
		DPRINTF("Did not find index\n");
		return -1;
	}
