#ifndef RC_PLATFORM_H
#define RC_PLATFORM_H
#include "riden_core.h"
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION rc_mutex;
typedef CONDITION_VARIABLE rc_cond;
typedef HANDLE rc_thread;
#else
#include <pthread.h>
typedef pthread_mutex_t rc_mutex;
typedef pthread_cond_t rc_cond;
typedef pthread_t rc_thread;
#endif
RC_API void rc_mutex_init(rc_mutex *);
RC_API void rc_mutex_destroy(rc_mutex *);
RC_API void rc_lock(rc_mutex *);
RC_API void rc_unlock(rc_mutex *);
RC_API void rc_cond_init(rc_cond *);
RC_API void rc_cond_destroy(rc_cond *);
RC_API void rc_broadcast(rc_cond *);
RC_API void rc_wait(rc_cond *, rc_mutex *);
RC_API void rc_timedwait(rc_cond *, rc_mutex *, unsigned ms);
RC_API int rc_thread_start(rc_thread *, void *(*fn)(void *), void *);
RC_API void rc_thread_join(rc_thread);
RC_API void rc_sleep_ns(uint64_t);
#endif
