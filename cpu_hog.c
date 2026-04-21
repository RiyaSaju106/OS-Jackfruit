/*
 * cpu_hog.c - CPU-bound workload for scheduler experiments.
 *
 * Usage:
 *   /cpu_hog [seconds]
 *
 * The program burns CPU and prints progress once per second so students
 * can compare completion times and responsiveness under different
 * priorities or CPU-affinity settings.
 *
 * If you copy this binary into an Alpine rootfs, make sure it is built in a
 * format that can run there.
 */

#include <stdio.h>
#include <unistd.h>

int main() {
    printf("Starting CPU hog...\n");

    volatile unsigned long long i = 0;

    while (1) {
        i++;
        if (i % 1000000000 == 0) {
            printf("Still running...\n");
        }
    }

    return 0;
}
