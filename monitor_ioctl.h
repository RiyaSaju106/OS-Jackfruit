#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <sys/types.h>
#endif

#define MONITOR_MAGIC 'M'

typedef struct monitor_request {
    pid_t pid;                  // host PID of container process
    unsigned long soft_limit;   // in MiB
    unsigned long hard_limit;   // in MiB
} monitor_request_t;

#define REGISTER_CONTAINER   _IOW(MONITOR_MAGIC, 1, monitor_request_t)
#define UNREGISTER_CONTAINER _IOW(MONITOR_MAGIC, 2, pid_t)

#endif
