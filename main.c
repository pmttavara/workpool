
#define _CRT_SECURE_NO_WARNINGS

#include "spall_auto.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>

#if !_WIN32

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#else

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

typedef SSIZE_T ssize_t;
typedef HANDLE pthread_t;
typedef CRITICAL_SECTION pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;

#define pthread_create(thread, _, routine, userdata) (*(thread) = CreateThread(NULL, 0, (DWORD (*)(void *))routine, userdata, 0, NULL))
#define pthread_join(thread, _) WaitForSingleObject(thread, INFINITE)
#define pthread_mutex_init(m, _) InitializeCriticalSection(m)
#define pthread_mutex_lock(m) EnterCriticalSection(m)
#define pthread_mutex_trylock(m) TryEnterCriticalSection(m)
#define pthread_mutex_unlock(m) LeaveCriticalSection(m)
#define pthread_cond_init(c, _) InitializeConditionVariable(c)
#define pthread_cond_broadcast(c) WakeAllConditionVariable(c)
#define pthread_cond_wait(c, m) SleepConditionVariableCS(c, m, INFINITE)
#define pthread_cond_signal(c) WakeConditionVariable(c)
#define sched_yield() SwitchToThread()

#define _Atomic volatile
#define _Thread_local __declspec(thread)

static inline void usleep(__int64 usec) {
    __int64 ft = -10 * usec;

    HANDLE timer = CreateWaitableTimer(NULL, TRUE, NULL);
    SetWaitableTimer(timer, (LARGE_INTEGER *)&ft, 0, NULL, NULL, 0);
    WaitForSingleObject(timer, INFINITE);
    CloseHandle(timer);
}

#endif


#define THREAD_QUEUE_CAP 16000
typedef ssize_t tpool_task_proc(void *data);
typedef struct TPoolTask {
	tpool_task_proc  *do_work;
	void             *args;
} TPoolTask;

typedef struct Thread {
	pthread_t thread;
	int idx;

	TPoolTask *queue;
	size_t capacity;
	_Atomic uint64_t head;
	_Atomic uint64_t tail;
	pthread_mutex_t queue_lock;

	struct TPool *pool;
} Thread;

typedef struct TPool {
	struct Thread *threads;

	int thread_count;
	bool running;

	pthread_cond_t tasks_available;
	pthread_mutex_t task_lock;

	_Atomic uint64_t tasks_done;
	_Atomic uint64_t tasks_total;
} TPool;

_Thread_local Thread *current_thread = NULL;
_Thread_local int work_count = 0;

void mutex_init(pthread_mutex_t *mut) {
	pthread_mutex_init(mut, NULL);
}
void mutex_lock(pthread_mutex_t *mut) {
	pthread_mutex_lock(mut);
}
void mutex_unlock(pthread_mutex_t *mut) {
	pthread_mutex_unlock(mut);
}
int mutex_trylock(pthread_mutex_t *mut) {
	return pthread_mutex_trylock(mut);
}
void cond_init(pthread_cond_t *cond) {
	pthread_cond_init(cond, NULL);
}
void cond_broadcast(pthread_cond_t *cond) {
	pthread_cond_broadcast(cond);
}
void cond_signal(pthread_cond_t *cond) {
	pthread_cond_signal(cond);
}
void cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
	pthread_cond_wait(cond, mutex);
}

void tqueue_push(Thread *thread, TPoolTask task) {
	if ((thread->head - thread->tail) >= thread->capacity) {
		printf("Task queue is too full!!\n");
		exit(1);
	}

	size_t idx = thread->head % thread->capacity;
	thread->queue[idx] = task;
	thread->head++;
	thread->pool->tasks_total++;

	cond_broadcast(&thread->pool->tasks_available);
}

void tqueue_push_safe(Thread *thread, TPoolTask task) {
	mutex_lock(&thread->queue_lock);
	tqueue_push(thread, task);
	mutex_unlock(&thread->queue_lock);
}

TPoolTask *tqueue_pop(Thread *thread) {
	if (thread->tail >= thread->head) {
		return NULL;
	}

	size_t idx = thread->tail % thread->capacity;
	TPoolTask *task = &thread->queue[idx];
	thread->tail++;
	return task;
}

TPoolTask *tqueue_pop_safe(Thread *thread) {
	mutex_lock(&thread->queue_lock);
	TPoolTask *task = tqueue_pop(thread);
	mutex_unlock(&thread->queue_lock);
	return task;
}

void thread_sleep(void) {
	sched_yield();
}

void *tpool_worker(void *ptr) {
	current_thread = (Thread *)ptr;
	TPool *pool = current_thread->pool;
	spall_auto_thread_init(current_thread->idx, SPALL_DEFAULT_BUFFER_SIZE, SPALL_DEFAULT_SYMBOL_CACHE_SIZE);

	for (;;) {
		work_start:

		if (!pool->running) {
			break;
		}

		// If we've got tasks to process, work through them
		while (current_thread->head > current_thread->tail) {
			TPoolTask *task = tqueue_pop_safe(current_thread);
			if (!task) {
				break;
			}

			task->do_work(task->args);
			pool->tasks_done++;
		}

		// If there's still work somewhere and we don't have it, steal it
		if ((pool->tasks_done < pool->tasks_total) && (current_thread->head == current_thread->tail)) {
			int idx = current_thread->idx;
			for (int i = 0; i < pool->thread_count; i++) {
				if (pool->tasks_done == pool->tasks_total) {
					break;
				}

				idx = (idx + 1) % pool->thread_count;
				Thread *thread = &pool->threads[idx];

				if (thread->head > thread->tail) {
					int ret = mutex_trylock(&thread->queue_lock);
					if (ret) {
						continue;
					}

					TPoolTask *task = tqueue_pop(thread);
					mutex_unlock(&thread->queue_lock);
					if (!task) {
						continue;
					}

					task->do_work(task->args);
					pool->tasks_done++;
					goto work_start;
				}
			}
		}

		// if we've done all our work, there's nothing to steal, but work is still outstanding, go to sleep
		if (pool->tasks_done < pool->tasks_total) {
			cond_wait(&pool->tasks_available, &pool->task_lock);
			mutex_unlock(&pool->task_lock);
		}
	}

	spall_auto_thread_quit();
	return NULL;
}

void tpool_wait(TPool *pool) {
	while (pool->tasks_done < pool->tasks_total) {

		// if we've got tasks on our queue, run them
		while (current_thread->head > current_thread->tail) {
			TPoolTask *task = tqueue_pop_safe(current_thread);
			if (!task) {
				break;
			}

			task->do_work(task->args);
			pool->tasks_done++;
		}

		if (pool->tasks_done == pool->tasks_total) {
			break;
		}

		thread_sleep();
	}
}

void thread_start(Thread *thread) {
	pthread_create(&thread->thread, NULL, tpool_worker, (void *)thread);
}
void thread_end(Thread thread) {
	pthread_join(thread.thread, NULL);
	free(thread.queue);
}

void thread_init(TPool *pool, Thread *thread, int idx) {
	mutex_init(&thread->queue_lock);
	thread->capacity = THREAD_QUEUE_CAP;
	thread->queue = malloc(sizeof(TPoolTask) * thread->capacity);
	thread->head = 0;
	thread->tail = 0;
	thread->pool = pool;
	thread->idx = idx;
}

TPool *tpool_init(int child_thread_count) {
	TPool *pool = calloc(sizeof(TPool), 1);

	int thread_count = child_thread_count + 1;

	pool->thread_count = thread_count;
	pool->threads = calloc(sizeof(Thread), pool->thread_count);
	cond_init(&pool->tasks_available);
	mutex_init(&pool->task_lock);
	pool->running = true;

	// setup the main thread
	thread_init(pool, &pool->threads[0], 0);
	current_thread = &pool->threads[0];

	for (int i = 1; i < pool->thread_count; i++) {
		thread_init(pool, &pool->threads[i], i);
		thread_start(&pool->threads[i]);
	}

	return pool;
}

void tpool_destroy(TPool *pool) {
	pool->running = false;
	for (int i = 1; i < pool->thread_count - 1; i++) {
		Thread *thread = &pool->threads[i];
		cond_broadcast(&pool->tasks_available);
		thread_end(pool->threads[i]);
	}

	free(pool->threads[0].queue);
	free(pool->threads);
	free(pool);
}

ssize_t little_work(void *args) {
	size_t count = (size_t)args;

	// this is my workload. enjoy
	int sleep_time = rand() % 201;
	usleep(sleep_time);

	if (current_thread->pool->tasks_total < 10000) {
		mutex_lock(&current_thread->queue_lock);
		for (int i = 0; i < 5; i++) {
			TPoolTask task;
			task.do_work = little_work;
			task.args = (void *)(uint64_t)(count);
			tqueue_push(current_thread, task);
		}
		mutex_unlock(&current_thread->queue_lock);
	}
	return 0;
}


int main(void) {
	srand(1);
	spall_auto_init("pool_test.spall");
	spall_auto_thread_init(0, SPALL_DEFAULT_BUFFER_SIZE, SPALL_DEFAULT_SYMBOL_CACHE_SIZE);

	TPool *pool = tpool_init(4);

	int initial_task_count = 10;

	mutex_lock(&current_thread->queue_lock);
	for (int i = 0; i < initial_task_count; i++) {
		TPoolTask task;
		task.do_work = little_work;
		task.args = (void *)(uint64_t)(i + 1);
		tqueue_push(current_thread, task);
	}
	mutex_unlock(&current_thread->queue_lock);

	tpool_wait(pool);
	tpool_destroy(pool);

	spall_auto_thread_quit();
	spall_auto_quit();
}

#define SPALL_AUTO_IMPLEMENTATION
#define SPALL_BUFFER_PROFILING
#define SPALL_BUFFER_PROFILING_GET_TIME() __rdtsc()
#include "spall_auto.h"
