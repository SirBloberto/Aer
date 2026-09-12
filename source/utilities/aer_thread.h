#ifndef AER_THREAD_H
#define AER_THREAD_H

/* Threads only exist when the scheduler is built to use them (AER_HEAP_REF_TLS, which is also what
   makes the per-thread VM state per-thread). Without it these are empty, so a single-threaded build
   carries no locking and no pthread dependency at all. */
#ifdef AER_HEAP_REF_TLS
#include <pthread.h>

typedef pthread_mutex_t aer_mutex;
#define AER_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define aer_mutex_init(m) pthread_mutex_init((m), NULL)
#define aer_mutex_destroy(m) pthread_mutex_destroy(m)
#define aer_mutex_lock(m) pthread_mutex_lock(m)
#define aer_mutex_unlock(m) pthread_mutex_unlock(m)

typedef pthread_t aer_thread;
#define aer_thread_start(t, fn, arg) pthread_create((t), NULL, (fn), (arg))
#define aer_thread_join(t) pthread_join((t), NULL)

#else

typedef char aer_mutex;
#define AER_MUTEX_INIT 0
#define aer_mutex_init(m) ((void)(m))
#define aer_mutex_destroy(m) ((void)(m))
#define aer_mutex_lock(m) ((void)(m))
#define aer_mutex_unlock(m) ((void)(m))

#endif

/* How many workers to run tasks on. 1 without thread support, so aer_scheduler_run keeps its
   original single-threaded shape rather than needing a second code path. */
unsigned int aer_thread_hardware_workers(void);

#endif
