package io.riden.core;
interface IRidenCore {
    long[] snapshot();
    int[] read(int start, int count, int maxAgeMs);
    int write(int start, in int[] values);
}
