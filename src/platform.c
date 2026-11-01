#include "platform.h"
#include "riden_core.h"
#include <stdlib.h>
#ifdef _WIN32
uint64_t rc_monotonic_ns(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart / f.QuadPart) * 1000000000ULL +
           (uint64_t)(t.QuadPart % f.QuadPart) * 1000000000ULL / (uint64_t)f.QuadPart;
}
void rc_mutex_init(rc_mutex *m) {
    InitializeCriticalSection(m);
}
void rc_mutex_destroy(rc_mutex *m) {
    DeleteCriticalSection(m);
}
void rc_lock(rc_mutex *m) {
    EnterCriticalSection(m);
}
void rc_unlock(rc_mutex *m) {
    LeaveCriticalSection(m);
}
void rc_cond_init(rc_cond *c) {
    InitializeConditionVariable(c);
}
void rc_cond_destroy(rc_cond *c) {
    (void)c;
}
void rc_broadcast(rc_cond *c) {
    WakeAllConditionVariable(c);
}
void rc_wait(rc_cond *c, rc_mutex *m) {
    SleepConditionVariableCS(c, m, INFINITE);
}
void rc_timedwait(rc_cond *c, rc_mutex *m, unsigned ms) {
    SleepConditionVariableCS(c, m, ms);
}
struct startup {
    void *(*fn)(void *);
    void *arg;
};
static DWORD WINAPI thunk(void *p) {
    struct startup s = *(struct startup *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}
int rc_thread_start(rc_thread *t, void *(*fn)(void *), void *arg) {
    struct startup *s = malloc(sizeof(*s));
    if (!s)
        return -1;
    s->fn = fn;
    s->arg = arg;
    *t = CreateThread(NULL, 0, thunk, s, 0, NULL);
    if (!*t) {
        free(s);
        return -1;
    }
    return 0;
}
void rc_thread_join(rc_thread t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
void rc_sleep_ns(uint64_t ns) {
    HANDLE h = CreateWaitableTimerExA(NULL, NULL, 0x00000002, TIMER_ALL_ACCESS);
    if (!h)
        h = CreateWaitableTimer(NULL, TRUE, NULL);
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)((ns + 99) / 100);
    if (h) {
        SetWaitableTimer(h, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    } else
        Sleep((DWORD)((ns + 999999) / 1000000));
}
#else
#include <errno.h>
#include <time.h>
uint64_t rc_monotonic_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}
void rc_mutex_init(rc_mutex *m) {
    pthread_mutex_init(m, NULL);
}
void rc_mutex_destroy(rc_mutex *m) {
    pthread_mutex_destroy(m);
}
void rc_lock(rc_mutex *m) {
    pthread_mutex_lock(m);
}
void rc_unlock(rc_mutex *m) {
    pthread_mutex_unlock(m);
}
void rc_cond_init(rc_cond *c) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
}
void rc_cond_destroy(rc_cond *c) {
    pthread_cond_destroy(c);
}
void rc_broadcast(rc_cond *c) {
    pthread_cond_broadcast(c);
}
void rc_wait(rc_cond *c, rc_mutex *m) {
    pthread_cond_wait(c, m);
}
void rc_timedwait(rc_cond *c, rc_mutex *m, unsigned ms) {
    uint64_t n = rc_monotonic_ns() + (uint64_t)ms * 1000000ULL;
    struct timespec t = {(time_t)(n / 1000000000ULL), (long)(n % 1000000000ULL)};
    pthread_cond_timedwait(c, m, &t);
}
int rc_thread_start(rc_thread *t, void *(*fn)(void *), void *arg) {
    return pthread_create(t, NULL, fn, arg);
}
void rc_thread_join(rc_thread t) {
    pthread_join(t, NULL);
}
void rc_sleep_ns(uint64_t ns) {
    struct timespec t = {(time_t)(ns / 1000000000ULL), (long)(ns % 1000000000ULL)};
    while (nanosleep(&t, &t) && errno == EINTR) {
    }
}
#endif
