#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

#define MONITOR_REGISTER   _IOW('k', 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW('k', 2, struct monitor_request)

struct monitor_request {
    pid_t pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char container_id[32];
};

#endif
