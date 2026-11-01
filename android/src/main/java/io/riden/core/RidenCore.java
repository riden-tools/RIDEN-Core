package io.riden.core;

import java.io.Closeable;
import java.io.IOException;

/** A single device owner. Call from background threads; read/write may block.
 * snapshot() only accesses cache. USB/Bluetooth implementations own framing-free bytes. */
public final class RidenCore implements Closeable {
    static { System.loadLibrary("riden_core"); }
    public interface Transport extends Closeable {
        int read(byte[] buffer, int timeoutMs) throws IOException;
        int write(byte[] data, int timeoutMs) throws IOException;
    }
    public static final int HOT = 0, STATUS = 1, FULL = 2;
    private final Transport transport;
    private long handle;
    public RidenCore(Transport transport, int mode, boolean writes, boolean multiwrite) {
        this.transport = transport;
        handle = nativeCreate(transport, mode, writes, multiwrite);
        if (handle == 0) throw new IllegalStateException("Cannot create RIDEN core");
    }
    private void check() { if (handle == 0) throw new IllegalStateException("Core closed"); }
    /** ABI v1: [version, seq, completedNs, requestNs, transactions, failures,
     * rejected, coalesced, cacheHits, queued, lastError, 121 raw registers,
     * 121 updatedNs]. Invalid registers are -1. Time is monotonic. */
    public synchronized long[] snapshot() { check(); return nativeSnapshot(handle); }
    public synchronized int[] read(int start, int count, int maxAgeMs) {
        check();
        if (start < 0 || count < 1 || count > 121 || start > 121 - count || maxAgeMs < 0)
            throw new IllegalArgumentException("Invalid range/age");
        return nativeRead(handle, start, count, maxAgeMs);
    }
    public synchronized int write(int start, int[] values) {
        check(); return nativeWrite(handle, start, values);
    }
    @Override public synchronized void close() throws IOException {
        if (handle != 0) { nativeDestroy(handle); handle = 0; transport.close(); }
    }
    private static native long nativeCreate(Transport transport, int mode, boolean writes,
                                            boolean multiwrite);
    private static native void nativeDestroy(long handle);
    private static native long[] nativeSnapshot(long handle);
    private static native int[] nativeRead(long handle, int start, int count, int age);
    private static native int nativeWrite(long handle, int start, int[] values);
}
