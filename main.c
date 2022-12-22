
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

#else

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

typedef SSIZE_T ssize_t;
typedef HANDLE pthread_t;
typedef CRITICAL_SECTION pthread_mutex_t;

#define pthread_create(thread, _, routine, userdata) (*(thread) = CreateThread(NULL, 0, (DWORD (*)(void *))routine, userdata, 0, NULL))
#define pthread_join(thread, _) WaitForSingleObject(thread, INFINITE)
#define pthread_mutex_init(m, _) InitializeCriticalSection(m)
#define pthread_mutex_lock(m) EnterCriticalSection(m)
#define pthread_mutex_unlock(m) LeaveCriticalSection(m)
#define sched_yield() SwitchToThread()

#define _Atomic volatile
#define _Thread_local __declspec(thread)

#endif


typedef ssize_t tpool_task_proc(void *data);
typedef struct TPoolTask {
	struct TPoolTask *next;
	tpool_task_proc  *do_work;
	void             *args;
} TPoolTask;

typedef struct Thread {
	pthread_t thread;
	int idx;

	TPoolTask *queue;
	pthread_mutex_t queue_lock;

	_Atomic uint64_t tasks_done;
	_Atomic uint64_t tasks_total;

	struct TPool *pool;
} Thread;

typedef struct TPool {
	struct Thread *threads;

	int thread_count;
	bool running;

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

void tqueue_push(Thread *thread, TPoolTask *task) {
	if (!thread->queue) {
		thread->queue = task;
		goto end;
	}

	TPoolTask *old_head = thread->queue;
	task->next = old_head;
	thread->queue = task;

end:
	thread->tasks_total++;
	thread->pool->tasks_total++;
}

void tqueue_push_safe(Thread *thread, TPoolTask *task) {
	mutex_lock(&thread->queue_lock);
	tqueue_push(thread, task);
	mutex_unlock(&thread->queue_lock);
}

TPoolTask *tqueue_pop(Thread *thread) {
	if (!thread->queue) {
		return NULL;
	}

	TPoolTask *task = thread->queue;
	thread->queue = task->next;
	return task;
}

TPoolTask *tqueue_pop_safe(Thread *thread) {
	mutex_lock(&thread->queue_lock);
	TPoolTask *task = tqueue_pop(thread);
	mutex_unlock(&thread->queue_lock);
	return task;
}

void *tpool_worker(void *ptr) {
	current_thread = (Thread *)ptr;
	spall_auto_thread_init(current_thread->idx, SPALL_DEFAULT_BUFFER_SIZE, SPALL_DEFAULT_SYMBOL_CACHE_SIZE);

	for (;;) {
		if (!current_thread->pool->running) {
			break;
		}
		while (current_thread->tasks_done < current_thread->tasks_total) {
			uint64_t stale_done = current_thread->tasks_done;
			uint64_t stale_total = current_thread->tasks_total;

			TPoolTask *task = tqueue_pop_safe(current_thread);
			if (!task) {
				printf("WTF! %"PRIu64", %"PRIu64"\n", stale_done, stale_total);
				exit(0);
			}

			task->do_work(task->args);
			free(task);
			current_thread->pool->tasks_done++;
			current_thread->tasks_done++;
		}
		if (current_thread->tasks_done == current_thread->tasks_total) {
			sched_yield();
		}
	}

	spall_auto_thread_quit();
	return NULL;
}
void thread_start(Thread *thread) {
	pthread_create(&thread->thread, NULL, tpool_worker, (void *)thread);
}
void thread_end(Thread thread) {
	pthread_join(thread.thread, NULL);
}

void thread_init(TPool *pool, Thread *thread, int idx) {
	mutex_init(&thread->queue_lock);
	thread->queue = NULL;
	thread->pool = pool;
	thread->idx = idx;
}

TPool *tpool_init(int child_thread_count) {
	TPool *pool = calloc(sizeof(TPool), 1);

	int thread_count = child_thread_count + 1;

	pool->thread_count = thread_count;
	pool->threads = calloc(sizeof(Thread), pool->thread_count);
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
		thread_end(pool->threads[i]);
	}
	free(pool->threads);
	free(pool);
}

ssize_t little_work(void *args) {
	size_t count = (size_t)args;

	if (current_thread->pool->tasks_total < 10000) {

		TPool *pool = current_thread->pool;
		int64_t sibling_idx = (rand()) % pool->thread_count;
		Thread *sibling_thread = &pool->threads[sibling_idx];

		TPoolTask *task = calloc(sizeof(TPoolTask), 1);
		task->do_work = little_work;
		task->args = (void *)(uint64_t)(count);
		
		tqueue_push_safe(sibling_thread, task);
	}
	return 0;
}

void tpool_wait(TPool *pool) {
	while (pool->tasks_done < pool->tasks_total) {
		while (current_thread->tasks_done < current_thread->tasks_total) {
			TPoolTask *task = tqueue_pop_safe(current_thread);
			if (!task) {
				break;
			}

			task->do_work(task->args);
			pool->tasks_done++;
			current_thread->tasks_done++;
		}
		if (current_thread->tasks_done == current_thread->tasks_total) {
			sched_yield();
		}
	}
}

int main(void) {
	srand(1);
	spall_auto_init("pool_test.spall");
	spall_auto_thread_init(0, SPALL_DEFAULT_BUFFER_SIZE, SPALL_DEFAULT_SYMBOL_CACHE_SIZE);

	TPool *pool = tpool_init(8);

	int initial_task_count = 10;

	mutex_lock(&current_thread->queue_lock);
	for (int i = 0; i < initial_task_count; i++) {
		TPoolTask *task = calloc(sizeof(TPoolTask), 1);
		task->do_work = little_work;
		task->args = (void *)(uint64_t)(i + 1);
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
