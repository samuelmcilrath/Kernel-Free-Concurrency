#include <assert.h>
#include <sys/types.h>
#include <stdlib.h>
#include "kfc.h"
#include <ucontext.h>
#include <pthread.h>
#include "queue.h"
#include <sys/ucontext.h> // For REG_RIP
#include "valgrind/valgrind.h"
#include <unistd.h>
//TODO need to make some sort of kernel storage 
//right now all the kernel threads are sharing the same scheduler context; 

static int inited = 0;

ucontext_t scheduler; //context that acts as a scheduler
static int id_count; //counter for current thread id
tid_t curr_id; //t id for the running thread 

tcb_t thread_storage[KFC_MAX_THREADS]; //pointer to global storage 
static queue_t free_ids; //q of free spots from thread storage if running thread exits and gives up id
queue_t thread_q; //fcfs q; this should be a queue of thread_id's not contexts

//m2m
kthread_mutex_t k_lock;
kthread_mutex_t q_lock;
kthread_mutex_t sem_lock;
kthread_mutex_t t_lock; //this is for thread storage and swap contexts
kthread_cond_t k_cond;
kthread_t k_ids[MAX_KTHREADS];
ktcb_t k_storage[MAX_KTHREADS];
tid_t k_curr_ids[MAX_KTHREADS];
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
int kfc_init(int kthreads, int quantum_us)
{
    assert(!inited);
    num_kthreads = kthreads;
    inited = 1;

    //initialize kstorage main thread
    k_storage[0].k_id = kthread_self();
    getcontext(&k_storage[0].scheduler);
    k_storage[0].scheduler.uc_stack.ss_sp = malloc(KFC_DEF_STACK_SIZE);
    k_storage[0].scheduler.uc_stack.ss_size = KFC_DEF_STACK_SIZE;
    k_storage[0].scheduler.uc_stack.ss_flags = 0;
    k_storage[0].curr_id = 0;
    // what to set link to?
    // k_storage[0].scheduler.uc_link = &thread_storage[0].context;
    makecontext(&k_storage[0].scheduler, kfc_schedule, 0);
    k_storage[0].scheduler.uc_link = NULL;
    /*initialize variables*/
    id_count = 0;
    k_storage[0].curr_id = id_count++; //main thread gets id 0
    // main thread
    thread_storage[0].id = k_storage[0].curr_id;
    thread_storage[0].fin = 0;
    thread_storage[0].join_id = -1;

    getcontext(&thread_storage[0].context);
    thread_storage[0].context.uc_stack.ss_sp = malloc(KFC_DEF_STACK_SIZE);
    if (!thread_storage[0].context.uc_stack.ss_sp) {
        perror("malloc main thread stack");
        return -1;
    }
    VALGRIND_STACK_REGISTER(thread_storage[0].context.uc_stack.ss_sp,thread_storage[0].context.uc_stack.ss_sp + KFC_DEF_STACK_SIZE);
    thread_storage[0].context.uc_stack.ss_size = KFC_DEF_STACK_SIZE;
    thread_storage[0].context.uc_stack.ss_flags = 0;
    // Set up its link so that if it returns, control goes to the scheduler.
    thread_storage[0].context.uc_link = NULL;

    //initialize id for later
    for (int i = 1; i < KFC_MAX_THREADS; i++)
        thread_storage[i].id = -1;
    
    //initialize q's 
    queue_init(&free_ids);
    queue_init(&thread_q);
    
    kthread_mutex_init(&k_lock);
    kthread_mutex_init(&t_lock);
    kthread_mutex_init(&q_lock);
    kthread_mutex_init(&sem_lock);
    kthread_cond_init(&k_cond);
    
    DPRINTF("Kthreads num: %d\n", kthreads);
    //initialize kthreads
    if (kthreads > 1) {
        for (int i = 1; i < kthreads; i++) {
            //k_ids[i] = i;
            int *arg = malloc(sizeof(int));
            *arg = i; // Store the index in heap memory
            DPRINTF("kthread %d: creating kern with index %d\n", kthread_self(), k_ids[i]);
            kthread_create(&k_storage[i].k_id, kfc_ktrampoline, arg);
        }
    }
    
    //setcontext(&k_storage[0].scheduler);
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
 */
void kfc_teardown(void)
{
    assert(inited);
    
    inited = 0;

    // free(scheduler.uc_stack.ss_sp); //free scheduler

    shutdown = 1;

    // signal any waiting threads 
    for (int i = 0; i < num_kthreads; i++)
        kthread_cond_broadcast(&k_cond);
    
    kthread_cond_destroy(&k_cond);
    kthread_mutex_destroy(&k_lock);
}

/**
 * essentially does what init does but for other kthreads
 * needed in order to properly get kthread_self value
 * 
 * @param index  index into the k_storage to store kthread info
 * maybe change return val to 0?
 */
void *kfc_ktrampoline(void *arg)
{
    //int index = kfc_find_index();
    int index = *(int *)arg;
    free(arg);
    DPRINTF("kthread %d: TRAMPOLINE - kindex:%d\n", kthread_self(), index);
        
    k_storage[index].k_id = kthread_self();

    // scheduler setup
    getcontext(&k_storage[index].scheduler);
    k_storage[index].scheduler.uc_stack.ss_sp = malloc(KFC_DEF_STACK_SIZE);
    k_storage[index].scheduler.uc_stack.ss_size = KFC_DEF_STACK_SIZE;
    k_storage[index].scheduler.uc_stack.ss_flags = 0;
    k_storage[index].scheduler.uc_link = NULL;
    VALGRIND_STACK_REGISTER(k_storage[index].scheduler.uc_stack.ss_sp, k_storage[index].scheduler.uc_stack.ss_sp + KFC_DEF_STACK_SIZE);
    
    makecontext(&k_storage[index].scheduler, kfc_schedule, 0);
    kthread_mutex_lock(&q_lock);
    DPRINTF("kthread %d: tramp - Q locked !!!\n", kthread_self());
    setcontext(&k_storage[index].scheduler);

    return NULL;
}

/**
 * Creates a new user thread which executes the provided function concurrently.
 * It is left up to the implementation to decide whether the calling thread
 * continues to execute or the new thread takes over immediately.
 *
 *
 * @param ptid[out]   Pointer to a tid_t variable in which to store the new thread's ID
 * @param start_func  Thread main function
 * @param arg         Argument to be passed to the thread main function
 * @param stack_base  Location of the thread's stack if already allocated, or NULL if requesting that the library allocate it dynamically
 * @param stack_size  Size (in bytes) of the thread's stack, or 0 to use the default thread stack size KFC_DEF_STACK_SIZE
 *
 * @return 0 if successful, nonzero on failure
 */
int kfc_create(tid_t *ptid, void *(*start_func)(void *), void *arg,
               caddr_t stack_base, size_t stack_size)
{
    int index = kfc_find_index();

    // deal with thread id
    if (free_ids.size > 0) {
        DPRINTF("kthread %d: SHOULD NEVER PRINT free_ids\n", kthread_self());
    } else {
        *ptid = id_count++;
        thread_storage[*ptid].id = *ptid;
    }

    DPRINTF("kthread %d: CREATE - kindex:%d ptid:%d\n", kthread_self(), index, *ptid);
    
    int err = 0; // check err
    err = getcontext(&thread_storage[*ptid].context);
    if (err == -1) {
        perror("getcontext err");
        return -1;
    }

    if (stack_size == 0)
        stack_size = KFC_DEF_STACK_SIZE;
    
    if (stack_base == NULL) {
        thread_storage[*ptid].context.uc_stack.ss_sp = malloc(stack_size);
        thread_storage[*ptid].context.uc_stack.ss_size = stack_size;
    }
    else {
        thread_storage[*ptid].context.uc_stack.ss_sp = stack_base;
        thread_storage[*ptid].context.uc_stack.ss_size = stack_size;
    }
    
    VALGRIND_STACK_REGISTER(thread_storage[*ptid].context.uc_stack.ss_sp,
                              thread_storage[*ptid].context.uc_stack.ss_sp + stack_size);
    thread_storage[*ptid].context.uc_stack.ss_flags = 0;
    thread_storage[*ptid].fin = 0;
    thread_storage[*ptid].join_id = -1;

    // make context
    makecontext(&thread_storage[*ptid].context, (void (*)())kfc_handle, 2, start_func, arg);

    assert(thread_storage[*ptid].context.uc_stack.ss_sp != NULL);
    
    // add new thread to the queue
    kthread_mutex_lock(&q_lock);

    DPRINTF("kthread %d: create - Q locked !!!\n", kthread_self());
    DPRINTF("kthread %d: enq @create id %d\n", kthread_self(), thread_storage[*ptid].id);

    queue_enqueue(&thread_q, &thread_storage[*ptid].id);
    kthread_cond_broadcast(&k_cond);
    kthread_mutex_unlock(&q_lock);
	

    DPRINTF("kthread %d: create - Q Unlocked !!!\n", kthread_self());
    
    return 0;
}

/**
 * Function to handle making the context and exiting threads 
 */
void kfc_handle(void *(*start_func)(void *), void *arg)
{
    DPRINTF("kthread %d: HANDLE\n", kthread_self());
    void *ret = start_func(arg);
    kfc_exit(ret);
    abort(); // shouldn't reach
}

/**
 * Exits the calling thread.  This should be the same thing that happens when
 * the thread's start_func returns.
 *
 * @param ret  Return value from the thread
 */
void kfc_exit(void *ret)
{
    int index = kfc_find_index();
    DPRINTF("kthread %d: EXIT - kindex:%d pid:%d\n", kthread_self(), index, thread_storage[k_storage[index].curr_id].id);
    assert(inited);
    
    thread_storage[k_storage[index].curr_id].ret = ret;
    thread_storage[k_storage[index].curr_id].fin = 1;
    

    if (thread_storage[k_storage[index].curr_id].join_id != -1) {
        kthread_mutex_lock(&q_lock);

        DPRINTF("kthread %d: exit - Q locked !!!\n", kthread_self());
        DPRINTF("kthread %d: Enq @exit join id %d\n", kthread_self(), thread_storage[k_storage[index].curr_id].join_id);

        queue_enqueue(&thread_q, &thread_storage[thread_storage[k_storage[index].curr_id].join_id].id);

        kthread_cond_broadcast(&k_cond);
        kthread_mutex_unlock(&q_lock);
		
        DPRINTF("kthread %d: exit - Q Unlocked !!!\n", kthread_self());
    }
    kthread_mutex_lock(&q_lock);

    DPRINTF("kthread %d: exit bot - Q locked !!!\n", kthread_self());

    setcontext(&k_storage[index].scheduler);
}

/**
 * Waits for the thread specified by tid to terminate, retrieving that thread's
 * return value.  Returns immediately if the target thread has already
 * terminated, otherwise blocks.  Attempting to join a thread which already has
 * another thread waiting to join it, or attempting to join a thread which has
 * already been joined, results in undefined behavior.
 *
 * @param pret[out]  Pointer to a void * in which the thread's return value from kfc_exit should be stored, or NULL if the caller does not care.
 *
 * @return 0 if successful, nonzero on failure
 */
int kfc_join(tid_t tid, void **pret)
{
    int index = kfc_find_index();
    DPRINTF("kthread %d: JOIN - kindex:%d\n", kthread_self(), index);
    
    assert(inited);
    DPRINTF("curr: %d joining with tid: %d fin status: %d\n", k_storage[index].curr_id, tid, thread_storage[tid].fin);
    DPRINTF("Current kindex: %d\n", index);
    
    if (!thread_storage[tid].fin) {
        thread_storage[tid].join_id = k_storage[index].curr_id;
        kthread_mutex_lock(&q_lock);
        DPRINTF("kthread %d: join - Q locked !!!\n", kthread_self());
        swapcontext(&thread_storage[k_storage[index].curr_id].context, &k_storage[index].scheduler);
    }
    
    if (pret != NULL)
        *pret = thread_storage[tid].ret;
    
    return 0;
}

/**
 * Returns a small integer which identifies the calling thread.
 *
 * @return Thread ID of the currently executing thread
 */
tid_t kfc_self(void)
{
    assert(inited);
    return k_storage[kfc_find_index()].curr_id;
}

/**
 * Causes the calling thread to yield the processor voluntarily.  This may
 * result in another thread being scheduled, but it does not preclude the
 * possibility of the same thread continuing if re-chosen by the scheduling algorithm.
 */
void kfc_yield(void)
{
    int index = kfc_find_index();
    DPRINTF("kthread %d: YIELD - kindex:%d\n", kthread_self(), index);

    assert(inited);

    kthread_mutex_lock(&q_lock);
    DPRINTF("kthread %d: yield - Q locked !!!\n", kthread_self());
    DPRINTF("kthread %d: enq @yield id %d\n", kthread_self(), thread_storage[k_storage[index].curr_id].id);
    queue_enqueue(&thread_q, &thread_storage[k_storage[index].curr_id].id);
    kthread_cond_broadcast(&k_cond);
    swapcontext(&thread_storage[k_storage[index].curr_id].context, &k_storage[index].scheduler);
}

/**
 * Initializes a user-level counting semaphore with a specific value.
 *
 * @param sem    Pointer to the semaphore to be initialized
 * @param value  Initial value for the semaphore's counter
 *
 * @return 0 if successful, nonzero on failure
 */
int kfc_sem_init(kfc_sem_t *sem, int value)
{
    DPRINTF("kthread %d: SEMINIT\n", kthread_self());
    kthread_mutex_lock(&sem_lock);
    assert(inited);
    sem->count = value;
    queue_init(&sem->q);
    kthread_mutex_unlock(&sem_lock);
    return 0;
}

/**
 * Increments the value of the semaphore.  Also known as up, signal, release, and V.
 *
 * @param sem  Pointer to the semaphore which the thread is releasing
 *
 * @return 0 if successful, nonzero on failure
 */
int kfc_sem_post(kfc_sem_t *sem)
{
    DPRINTF("kthread %d: SEM POST\n", kthread_self());
    kthread_mutex_lock(&sem_lock);
    assert(inited);
    sem->count++; //increment

    if (sem->q.size) {
        kthread_mutex_lock(&q_lock);
        DPRINTF("kthread %d: sempost - Q locked !!!\n", kthread_self());
        DPRINTF("kthread %d: Enq @sempost\n", kthread_self());
        queue_enqueue(&thread_q, queue_dequeue(&sem->q));
        kthread_cond_broadcast(&k_cond);
        kthread_mutex_unlock(&q_lock);
		
        DPRINTF("kthread %d: sem - Q Unlocked !!!\n", kthread_self());
    }
    
    kthread_mutex_unlock(&sem_lock);
    DPRINTF("kthread %d: sem - Q Unlocked !!!\n", kthread_self());
    return 0;
}

/**
 * Attempts to decrement the value of the semaphore (down/acquire). Blocks if count is not above 0.
 *
 * @param sem  Pointer to the semaphore which the thread wishes to acquire
 *
 * @return 0 if successful, nonzero on failure
 */
int kfc_sem_wait(kfc_sem_t *sem)
{
    int index = kfc_find_index();
    DPRINTF("kthread %d: SEMWAIT - kindex:%d\n", kthread_self(), index);
    assert(inited);
    
    kthread_mutex_lock(&sem_lock);
    while (sem->count <= 0) {
        queue_enqueue(&sem->q, &thread_storage[k_storage[index].curr_id].id);
        kthread_mutex_unlock(&sem_lock);
        
        kthread_mutex_lock(&q_lock);
        DPRINTF("kthread %d: sem wait - Q locked !!!\n", kthread_self());
        swapcontext(&thread_storage[k_storage[index].curr_id].context, &k_storage[index].scheduler);
        kthread_mutex_lock(&sem_lock);
    }
    sem->count--;
    kthread_mutex_unlock(&sem_lock);
    return 0;
}

/**
 * Frees any resources associated with a semaphore.
 *
 * @param sem  Pointer to the semaphore to be destroyed
 */
void kfc_sem_destroy(kfc_sem_t *sem)
{
    assert(inited);
    DPRINTF("kthread %d: SEMDESTROY\n", kthread_self());
    queue_destroy(&sem->q);
}

/**
 * Scheduler: selects the next thread to run (FCFS) and transfers control.
 * Should only be accessed by scheduler context.
 */
void kfc_schedule()
{
    DPRINTF("kthread %d: SCHEDULE (outside unlock)\n", kthread_self());
    kthread_mutex_unlock(&q_lock);
	
    DPRINTF("kthread %d: sched - Q Unlocked !!!\n", kthread_self());
    int index = kfc_find_index();
    
    kthread_mutex_lock(&q_lock);
    DPRINTF("kthread %d: schedule top - Q locked !!!\n", kthread_self());
    DPRINTF("kthread %d: SCHEDULE (inside unlock) - kindex:%d\n", kthread_self(), index);
    while (queue_size(&thread_q) == 0) {
        if (shutdown) {
            kthread_mutex_unlock(&q_lock);
			
            DPRINTF("kthread %d: sched shutdown - Q Unlocked !!!\n", kthread_self());
            exit(0);
        }
        DPRINTF("kthread %d: WAITING IN SCHEDULE - kindex:%d\n", kthread_self(), index);

        kthread_cond_wait(&k_cond, &q_lock);

        DPRINTF("kthread %d: SCHEDULE (cond **WAKEUP**) - kindex:%d\n", kthread_self(), index);
    }
    
    DPRINTF("kthread %d: SCHEDULE (past cond wait) - kindex:%d\n", kthread_self(), index);
    DPRINTF("kthread %d: ------ ENTERING THE SCHEDULER -----\n", kthread_self());

    assert(queue_size(&thread_q) != 0);
    k_storage[index].curr_id = *(int *)queue_dequeue(&thread_q);

    kthread_cond_broadcast(&k_cond);
    kthread_mutex_unlock(&q_lock);
	
    DPRINTF("kthread %d: sched(bot) - Q Unlocked !!!\n", kthread_self());
    
    DPRINTF("kthread %d: Setting user context to thread: %d\n", kthread_self(), k_storage[index].curr_id);

    setcontext(&thread_storage[k_storage[index].curr_id].context);
    
    DPRINTF("kthread %d: **SHOULD NEVER MAKE IT HERE (Scheduler)**\n", kthread_self());
}

/**
 * Searches through k_storage to find the index corresponding to kthread_self().
 * @return the index on success, or -1 on failure.
 */
int kfc_find_index()
{
    DPRINTF("kthread %d: FINDINDEX\n", kthread_self());
    int id = kthread_self();
    for (int i = 0; i < num_kthreads; i++) {
        if (k_storage[i].k_id == id)
            return i;
    }
    DPRINTF("kthread %d: Did not find index\n", kthread_self());
    return -1;
}
