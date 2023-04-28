// SPDX-License-Identifier: BSD-3-Clause

#include "os_threadpool.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

/* === SYNC ===*/

// Binary sempahore implementation using mutex and condition variable
// Very common implementation to avoid busy waiting
typedef struct sync_t {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	__uint8_t value; // 0 or 1
} sync_t;

// Used to signal that a thread has a task to execute
static sync_t *can_work;

/**
 * @brief Initialize a binary semaphore
 */
static void sync_init(sync_t *s, __uint8_t value)
{
	if (value != 0 && value != 1) {
		printf("[ERROR] [%s] Invalid value\n", __func__);
		return;
	}
	pthread_mutex_init(&s->lock, NULL);
	pthread_cond_init(&s->cond, NULL);
	s->value = value;
}

/**
 * @brief Notify a thread that it can execute a task
 */
static void sync_notify(sync_t *s)
{
	pthread_mutex_lock(&s->lock);
	s->value = 1;
	pthread_cond_signal(&s->cond);
	pthread_mutex_unlock(&s->lock);
}

/**
 * @brief Notify all threads that they can execute a task
 * will be used when the threadpool is stopped to avoid deadlocks
 */
static void sync_notify_all(sync_t *s)
{
	pthread_mutex_lock(&s->lock);
	s->value = 1;
	pthread_cond_broadcast(&s->cond);
	pthread_mutex_unlock(&s->lock);
}

/**
 * @brief Wait for a thread to receive a task
 */
static void sync_wait(sync_t *s)
{
	// This syntax is mandatory because of the way pthread_cond_wait works
	pthread_mutex_lock(&s->lock);
	while (s->value == 0) // will wait until sync_notify is called
		pthread_cond_wait(&s->cond, &s->lock); // This unlocks the mutex
	s->value = 0;
	pthread_mutex_unlock(&s->lock);
}

/* === TASK === */

/* Creates a task that thread must execute */
os_task_t *task_create(void *arg, void (*f)(void *))
{
	if (f == NULL) {
		printf("[ERROR] [%s] Function pointer is NULL\n", __func__);
		return NULL;
	}
	os_task_t *newTask = malloc(sizeof(os_task_t));

	if (newTask == NULL) {
		printf("[ERROR] [%s] Error allocating memory\n", __func__);
		return NULL;
	}
	newTask->argument = arg;
	newTask->task = f;

	return newTask;
}

/* Add a new task to threadpool task queue */
void add_task_in_queue(os_threadpool_t *tp, os_task_t *t)
{
	// Basic error checking
	if (tp == NULL) {
		printf("[ERROR] [%s] Threadpool is NULL\n", __func__);
		return;
	}
	if (t == NULL) {
		printf("[ERROR] [%s] Task is NULL\n", __func__);
		return;
	}
	// Allocate memory for the task node
	os_task_queue_t *newTaskNode = malloc(sizeof(os_task_queue_t));

	if (newTaskNode == NULL) {
		printf("[ERROR] [%s] Error allocating memory\n", __func__);
		return;
	}
	// Initialize the task node
	newTaskNode->task = t;
	newTaskNode->next = NULL;

	// Add the task to the front pushing the other tasks back
	pthread_mutex_lock(&tp->taskLock);
	if (tp->tasks == NULL) {
		tp->tasks = newTaskNode;
	} else {
		os_task_queue_t *aux = tp->tasks;

		tp->tasks = newTaskNode;
		newTaskNode->next = aux;
	}
	sync_notify(can_work);
	pthread_mutex_unlock(&tp->taskLock);
}

/* Get the head of task queue from threadpool */
os_task_t *get_task(os_threadpool_t *tp)
{
	if (tp == NULL) {
		printf("[ERROR] [%s] Threadpool is NULL\n", __func__);
		return NULL;
	}
	pthread_mutex_lock(&tp->taskLock);
	if (tp->tasks == NULL) {
		pthread_mutex_unlock(&tp->taskLock);
		return NULL;
	}
	// Get the first task from the queue
	os_task_t *current_task = tp->tasks->task;

	os_task_queue_t *toFree = tp->tasks;
	// Remove the task from the queue
	tp->tasks = tp->tasks->next;

	// free the memory of the task node
	free(toFree);

	// If there are tasks left, notify a thread that it can execute a task
	if (tp->tasks != NULL)
		sync_notify(can_work);

	pthread_mutex_unlock(&tp->taskLock);
	return current_task;
}

/* === THREAD POOL === */

static pthread_mutex_t threadCountLock;

/* Initialize the new threadpool */
os_threadpool_t *threadpool_create(unsigned int nTasks, unsigned int nThreads)
{
	// Classic memory allocation and error checking
	os_threadpool_t *newThreadpool = _os_threadpool_create();

	if (newThreadpool == NULL) {
		printf("[ERROR] [%s] Error creating threadpool\n", __func__);
		return NULL;
	}
	// Set the number of running threads and allocate memory for them
	newThreadpool->num_threads = nThreads;
	newThreadpool->threads = malloc(nThreads * sizeof(pthread_t));

	if (newThreadpool->threads == NULL) {
		printf("[ERROR] [%s] Error allocating memory\n", __func__);
		return NULL;
	}

	// Start the threads
	for (int i = 0; i < nThreads; i++)
		pthread_create(&newThreadpool->threads[i], NULL, thread_loop_function, (void *) newThreadpool);

	return newThreadpool;
}

os_threadpool_t *_os_threadpool_create(void)
{
	os_threadpool_t *newThreadpool = malloc(sizeof(os_threadpool_t));

	if (newThreadpool == NULL) {
		printf("[ERROR] [%s] Error allocating memory\n", __func__);
		return NULL;
	}
	newThreadpool->should_stop = 0;
	newThreadpool->num_threads = 0; // The number of running threads
	newThreadpool->threads = NULL;
	newThreadpool->tasks = NULL;
	pthread_mutex_init(&newThreadpool->taskLock, NULL);

	// Alocate memory for the sync variable
	can_work = (sync_t *) malloc(sizeof(sync_t));
	if (can_work == NULL) {
		printf("[ERROR] [%s] Error allocating memory\n", __func__);
		return NULL;
	}

	// initialise the thread count lock
	pthread_mutex_init(&threadCountLock, NULL);

	// Initialize the sync variable
	sync_init(can_work, 0);

	return newThreadpool;
}

/* Loop function for threads */
void *thread_loop_function(void *args)
{
	os_threadpool_t *tp = (os_threadpool_t *) args;
	// As long as the threadpool is not stopped
	while (!tp->should_stop) {
		// Wait for a task
		sync_wait(can_work);

		// Verify again if the threadpool is stopped
		if (tp->should_stop)
			break;

		// Get the task
		os_task_t *current_task = get_task(tp);

		if (current_task) {
			// Extract the function and the argument from the task
			void (*f)(void *) = current_task->task;

			void *arg = current_task->argument;

			// Execute the task
			f(arg);

			// Free the memory
			free(current_task);
		}
	}
	// decrease the number of running threads
	pthread_mutex_lock(&threadCountLock);
	tp->num_threads--;
	pthread_mutex_unlock(&threadCountLock);
	return NULL;
}

/* Stop the thread pool once a condition is met */
void threadpool_stop(os_threadpool_t *tp, int (*processingIsDone)(os_threadpool_t *))
{
	if (tp == NULL) {
		printf("[ERROR] [%s] Threadpool is NULL\n", __func__);
		return;
	}
	if (processingIsDone == NULL) {
		printf("[ERROR] [%s] Function pointer is NULL\n", __func__);
		return;
	}

	while (!processingIsDone(tp))
		continue;

	// notify all threads that they can stop
	tp->should_stop = 1;
	while (tp->num_threads > 0)
		sync_notify_all(can_work);

	// Join all threads
	for (int i = 0; i < tp->num_threads; i++)
		pthread_join(tp->threads[i], NULL);

	// Free the memory
	free(tp->threads);

	pthread_mutex_destroy(&tp->taskLock);
	pthread_mutex_destroy(&threadCountLock);
}
