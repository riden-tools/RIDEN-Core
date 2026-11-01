#include "riden_core.h"
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
struct android_core {
    JavaVM *vm;
    jobject transport;
    jmethodID read, write;
    rc_core *core;
};
static JNIEnv *environment(struct android_core *a, int *attached) {
    JNIEnv *e = NULL;
    *attached = 0;
    jint status = (*a->vm)->GetEnv(a->vm, (void **)&e, JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        if ((*a->vm)->AttachCurrentThread(a->vm, &e, NULL) != JNI_OK)
            return NULL;
        *attached = 1;
    } else if (status != JNI_OK)
        return NULL;
    return e;
}
static int transfer(struct android_core *a, uint8_t *data, size_t n, uint32_t timeout,
                    int writing) {
    int attached;
    JNIEnv *e = environment(a, &attached);
    if (!e)
        return -1;
    jbyteArray b = (*e)->NewByteArray(e, (jsize)n);
    int result = -1;
    if (b) {
        if (writing)
            (*e)->SetByteArrayRegion(e, b, 0, (jsize)n, (const jbyte *)data);
        jint k =
            (*e)->CallIntMethod(e, a->transport, writing ? a->write : a->read, b, (jint)timeout);
        if ((*e)->ExceptionCheck(e)) {
            (*e)->ExceptionClear(e);
        } else if (k >= 0 && (size_t)k <= n) {
            result = k;
            if (!writing && k)
                (*e)->GetByteArrayRegion(e, b, 0, k, (jbyte *)data);
        }
        (*e)->DeleteLocalRef(e, b);
    }
    if ((*e)->ExceptionCheck(e)) {
        (*e)->ExceptionClear(e);
        result = -1;
    }
    if (attached)
        (*a->vm)->DetachCurrentThread(a->vm);
    return result;
}
static int ar(void *a, uint8_t *b, size_t n, uint32_t ms) {
    return transfer(a, b, n, ms, 0);
}
static int aw(void *a, const uint8_t *b, size_t n, uint32_t ms) {
    return transfer(a, (uint8_t *)b, n, ms, 1);
}
static struct android_core *handle(jlong h) {
    return (struct android_core *)(intptr_t)h;
}
JNIEXPORT jlong JNICALL Java_io_riden_core_RidenCore_nativeCreate(JNIEnv *e, jclass cls,
                                                                  jobject transport, jint mode,
                                                                  jboolean writes, jboolean multi) {
    (void)cls;
    if (!transport || mode < 0 || mode > 2)
        return 0;
    struct android_core *a = calloc(1, sizeof(*a));
    if (!a)
        return 0;
    (*e)->GetJavaVM(e, &a->vm);
    a->transport = (*e)->NewGlobalRef(e, transport);
    jclass c = (*e)->GetObjectClass(e, transport);
    a->read = (*e)->GetMethodID(e, c, "read", "([BI)I");
    a->write = (*e)->GetMethodID(e, c, "write", "([BI)I");
    (*e)->DeleteLocalRef(e, c);
    if (!a->transport || !a->read || !a->write) {
        if (a->transport)
            (*e)->DeleteGlobalRef(e, a->transport);
        free(a);
        return 0;
    }
    rc_config cfg = rc_default_config();
    if (mode == 1) {
        cfg.poll_start = 4;
        cfg.poll_count = 16;
    }
    if (mode == 2) {
        cfg.poll_start = 0;
        cfg.poll_count = 121;
    }
    cfg.allow_writes = writes;
    cfg.allow_multiwrite = multi;
    rc_transport t = {a, ar, aw};
    a->core = rc_create(&t, &cfg);
    if (!a->core) {
        (*e)->DeleteGlobalRef(e, a->transport);
        free(a);
        return 0;
    }
    return (jlong)(intptr_t)a;
}
JNIEXPORT void JNICALL Java_io_riden_core_RidenCore_nativeDestroy(JNIEnv *e, jclass cls, jlong h) {
    (void)cls;
    struct android_core *a = handle(h);
    if (!a)
        return;
    rc_destroy(a->core);
    (*e)->DeleteGlobalRef(e, a->transport);
    free(a);
}
JNIEXPORT jlongArray JNICALL Java_io_riden_core_RidenCore_nativeSnapshot(JNIEnv *e, jclass cls,
                                                                         jlong h) {
    (void)cls;
    struct android_core *a = handle(h);
    if (!a)
        return NULL;
    rc_snapshot s;
    rc_snapshot_get(a->core, &s);
    jlong v[253] = {1,
                    (jlong)s.sequence,
                    (jlong)s.completed_ns,
                    (jlong)s.request_ns,
                    (jlong)s.transactions,
                    (jlong)s.failures,
                    (jlong)s.queue_rejected,
                    (jlong)s.coalesced,
                    (jlong)s.cache_hits,
                    (jlong)s.queued,
                    (jlong)s.last_error};
    for (unsigned i = 0; i < 121; i++) {
        v[11 + i] = s.valid[i] ? s.registers[i] : -1;
        v[132 + i] = (jlong)s.updated_ns[i];
    }
    jlongArray out = (*e)->NewLongArray(e, 253);
    if (out)
        (*e)->SetLongArrayRegion(e, out, 0, 253, v);
    return out;
}
JNIEXPORT jintArray JNICALL Java_io_riden_core_RidenCore_nativeRead(JNIEnv *e, jclass cls, jlong h,
                                                                    jint start, jint count,
                                                                    jint age) {
    (void)cls;
    struct android_core *a = handle(h);
    if (!a || start < 0 || count < 1 || count > 121 || start > 121 - count || age < 0)
        return NULL;
    uint16_t v[121];
    int status = rc_read(a->core, (uint16_t)start, (uint16_t)count, (uint32_t)age, v);
    if (status) {
        jclass ex = (*e)->FindClass(e, "java/lang/IllegalStateException");
        if (ex)
            (*e)->ThrowNew(e, ex, rc_error_string(status));
        return NULL;
    }
    jint copy[121];
    for (int i = 0; i < count; i++)
        copy[i] = v[i];
    jintArray out = (*e)->NewIntArray(e, count);
    if (out)
        (*e)->SetIntArrayRegion(e, out, 0, count, copy);
    return out;
}
JNIEXPORT jint JNICALL Java_io_riden_core_RidenCore_nativeWrite(JNIEnv *e, jclass cls, jlong h,
                                                                jint start, jintArray values) {
    (void)cls;
    struct android_core *a = handle(h);
    if (!a || !values || start < 0)
        return RC_RANGE;
    jsize count = (*e)->GetArrayLength(e, values);
    if (count < 1 || count > 121 || start > 121 - count)
        return RC_RANGE;
    jint copy[121];
    uint16_t v[121];
    (*e)->GetIntArrayRegion(e, values, 0, count, copy);
    for (int i = 0; i < count; i++) {
        if (copy[i] < 0 || copy[i] > 65535)
            return RC_RANGE;
        v[i] = (uint16_t)copy[i];
    }
    return rc_write(a->core, (uint16_t)start, (uint16_t)count, v);
}
