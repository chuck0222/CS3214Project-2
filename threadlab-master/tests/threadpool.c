/**
 * @file threadpool.c
 * @author Luke Janoshka (lukewarm)
 * @author Caleb Huck (calebcamo22)
 * @version 1.0
 * @date 2022-03-18
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>
#include <errno.h>
#include "list.h"
#include "threadpool.h"

/**
 * threadpool.h
 *
 * A work-stealing, fork-join thread pool.
 */

/* 
 * Opaque forward declarations. The actual definitions of these 
 * types will be local to your threadpool.c implementation.
 */
struct worker{
        struct list_elem elem;
        struct list * local_worker_tasks_list_p;
        pthread_t thread;
        pthread_mutex_t * local_worker_lock_p;
};

struct thread_pool{
        struct list thread_list;
        struct list task_list;
        pthread_mutex_t lock;
        pthread_cond_t cond;
        pthread_cond_t shutdown_cond;
        int nthreads;
        int ntasks;
        int shutdown; // shutdown flag: 0-working 1-shutdown
        int counter;
};

struct future{
        struct list_elem elem;
        struct thread_pool * pool;
        pthread_mutex_t lock;
        pthread_cond_t cond;
        fork_join_task_t task;
        void * data;
        void * value;
        int status; // 0-notDone 1-startedExecuting 2-done
};

//A worker's local list of tasks
static _Thread_local struct list worker_tasks_list;
static _Thread_local pthread_mutex_t worker_lock;
static _Thread_local int IS_MAIN_FLAG; //1 if thread is the main thread

/*
* Grabs future from pool if applicable, steals from others if empty and no future left in pool
*/
static void *threadpool_thread(void * threadpool) {
        struct thread_pool * pool = (struct thread_pool *)threadpool;

        // Thread local variables 
        list_init(&worker_tasks_list);
        pthread_mutex_init(&worker_lock, NULL);
        IS_MAIN_FLAG = 0;

        struct worker *thisWorker = calloc(1, sizeof(struct worker));
        thisWorker->thread = pthread_self();
        thisWorker->local_worker_tasks_list_p = &worker_tasks_list;
        thisWorker->local_worker_lock_p = &worker_lock;

        // Push this thread onto the thread_list so it can be accessed by everyone else
        pthread_mutex_lock(&pool->lock);
        list_push_back(&pool->thread_list, &thisWorker->elem);
        pool->counter++;
        if (pool->counter == pool->nthreads){
                pthread_cond_broadcast(&pool->shutdown_cond);
        }
        pthread_mutex_unlock(&pool->lock);

        for(;;){
                //wait for the next thread
                pthread_mutex_lock(&pool->lock);
                if(pool->ntasks == 0 && (pool->shutdown < 1)){
                        pthread_cond_wait(&pool->cond, &pool->lock);
                }

                //check if shutdown before doing anything
                if (pool->shutdown > 0){
                        pthread_mutex_unlock(&pool->lock);
                        break;
                }

                if (pool->ntasks > 0) { // global task is available
                        struct list_elem * e = list_pop_front(&pool->task_list);
                        struct future * future = list_entry(e, struct future, elem);
                        pool->ntasks--;
                        pthread_mutex_unlock(&pool->lock);

                        pthread_mutex_lock(&future->lock);
                        future->status = 1;
                        future->value = (*(future->task))(pool, future->data);
                        future->status = 2;
                        pthread_cond_broadcast(&future->cond);
                        pthread_mutex_unlock(&future->lock);
                } else { //work stealing
                        struct list_elem * wE = list_begin(&pool->thread_list);
                        struct list_elem * e;
                        bool foundTaskToSteal = false;
                        for(; wE != list_end(&pool->thread_list); wE = list_next(wE)){
                                struct worker * curWorker = list_entry(wE, struct worker, elem);
                                pthread_mutex_lock(curWorker->local_worker_lock_p);
                                        if (list_size(curWorker->local_worker_tasks_list_p) > 0){
                                                e = list_pop_back(curWorker->local_worker_tasks_list_p);
                                                foundTaskToSteal = true;
                                                pthread_mutex_unlock(curWorker->local_worker_lock_p);
                                                break;
                                        }
                                pthread_mutex_unlock(curWorker->local_worker_lock_p);
                        }
                        if (foundTaskToSteal) {
                                struct future * stolenFuture = list_entry(e, struct future, elem);
                                pthread_mutex_unlock(&pool->lock);

                                pthread_mutex_lock(&stolenFuture->lock);
                                stolenFuture->status = 1;
                                stolenFuture->value = (*(stolenFuture->task))(pool, stolenFuture->data);
                                stolenFuture->status = 2;
                                pthread_cond_broadcast(&stolenFuture->cond);
                                pthread_mutex_unlock(&stolenFuture->lock);
                        } else {
                              pthread_mutex_unlock(&pool->lock);  
                        }
                        
                }
        }
        pthread_exit(NULL);
        return(NULL);
}

/* Create a new thread pool with no more than n threads. */
struct thread_pool * thread_pool_new(int nthreads){

        IS_MAIN_FLAG = 1;

        struct thread_pool * pool;
        pool = (struct thread_pool *) malloc(sizeof(struct thread_pool));
        // Initialize
        pool->nthreads = nthreads;
        pool->ntasks = 0;
        pool->shutdown = 0;
        pool->counter = 0; 
        list_init(&pool->thread_list);
        list_init(&pool->task_list);
        pthread_mutex_init(&pool->lock, NULL);
        pthread_cond_init(&pool->cond, NULL);
        pthread_cond_init(&pool->shutdown_cond, NULL);
        pthread_mutex_lock(&pool->lock);

        // Start up all the threads
        for(int i = 0; i < nthreads; i++){
                pthread_t tempID;
                int rc = pthread_create(&tempID, NULL, threadpool_thread, pool);
                if (rc != 0) {
                   errno = rc;
                   perror("pthread_create");
                }
        }

        pthread_mutex_unlock(&pool->lock);

        return pool;
}

/* 
 * Shutdown this thread pool in an orderly fashion.  
 * Tasks that have been submitted but not executed may or
 * may not be executed.
 *
 * Deallocate the thread pool object before returning. 
 */
void thread_pool_shutdown_and_destroy(struct thread_pool * pool){
        pthread_mutex_lock(&pool->lock);
        if (pool->counter != pool->nthreads){
                pthread_cond_wait(&pool->shutdown_cond, &pool->lock);
        }
        pool->shutdown = 1;
        pthread_cond_broadcast(&pool->cond);
        pthread_mutex_unlock(&pool->lock);
        // Anything else that needs to happen...
        for(int i = 0; i < pool->nthreads; i++){
                pthread_mutex_lock(&pool->lock);
                struct list_elem * e = list_pop_front(&pool->thread_list);
                pthread_mutex_unlock(&pool->lock);
                struct worker *curThread = list_entry(e, struct worker, elem);
                pthread_join(curThread->thread, NULL);
                free(curThread);
        }

        // Deallocate pool
        free(pool);
}

/* 
 * Submit a fork join task to the thread pool and return a
 * future.  The returned future can be used in future_get()
 * to obtain the result.
 * 'pool' - the pool to which to submit
 * 'task' - the task to be submitted.
 * 'data' - data to be passed to the task's function
 *
 * Returns a future representing this computation.
 */
struct future * thread_pool_submit(struct thread_pool *pool, fork_join_task_t task, void * data){
        

        struct future * future;
        future = (struct future *) malloc(sizeof(struct future));
        future->status = 0;
        pthread_mutex_init(&future->lock, NULL);
        pthread_cond_init(&future->cond, NULL);
        future->task = task;
        future->data = data;
        future->pool = pool;

        if(IS_MAIN_FLAG) { // base thread - external submit
                pthread_mutex_lock(&pool->lock);
                list_push_back(&pool->task_list, &future->elem);
                pthread_cond_signal(&pool->cond); // wake up a thread if it was sleeping
                pool->ntasks++;
                pthread_mutex_unlock(&pool->lock);
        }
        else { // worker thread - internal submit

                pthread_mutex_lock(&future->lock);
                pthread_mutex_lock(&worker_lock);

                list_push_front(&worker_tasks_list, &future->elem);
                pthread_cond_broadcast(&pool->cond);

                pthread_mutex_unlock(&worker_lock);
                pthread_mutex_unlock(&future->lock);
        }
        return future;
}

/* Make sure that the thread pool has completed the execution
 * of the fork join task this future represents.
 *
 * Returns the value returned by this task.
 */
void * future_get(struct future * future){
        if(IS_MAIN_FLAG) { // base thread - external get
                pthread_mutex_lock(&future->lock);
                if(future->status < 2){ //wait until the data is filled out
                        pthread_cond_wait(&future->cond, &future->lock);
                }
                void * temp = future->value;
                pthread_mutex_unlock(&future->lock);
                return temp;
        }
        else { //worker thread - internal get
                while (future->status < 2){ // Our main task is waiting for another function now...
                        pthread_mutex_lock(&worker_lock);
                        if (list_size(&worker_tasks_list) > 0){
                                // if a subFuture exists that hasn't been done: start helping
                                struct list_elem * e = list_pop_front(&worker_tasks_list);
                                pthread_mutex_unlock(&worker_lock);
                                struct future * subFuture = list_entry(e, struct future, elem);
                                pthread_mutex_lock(&subFuture->lock);
                                subFuture->status = 1;
                                subFuture->value = (*(subFuture->task))(subFuture->pool, subFuture->data);
                                subFuture->status = 2;
                                pthread_cond_broadcast(&subFuture->cond);
                                pthread_mutex_unlock(&subFuture->lock);
                        } else {
                                // if there is no subFuture available (then another method MUST have stolen it): wait
                                pthread_mutex_unlock(&worker_lock);
                                pthread_mutex_lock(&future->lock);
                                if(future->status < 2){ //wait until the data is filled out
                                        pthread_cond_wait(&future->cond, &future->lock);
                                }
                                void * temp = future->value;
                                pthread_mutex_unlock(&future->lock);
                                return temp;  
                        }
                }
                pthread_mutex_lock(&future->lock);
                void * temp = future->value;
                pthread_mutex_unlock(&future->lock);
                return temp;
        }
}

/* Deallocate this future.  Must be called after future_get() */
void future_free(struct future * future){
        // Deallocate future
        pthread_mutex_lock(&future->lock);
        if(future->status < 2){ //wait until the data is filled out
                pthread_cond_wait(&future->cond, &future->lock);
        }
        pthread_mutex_unlock(&future->lock);
        free(future);
}