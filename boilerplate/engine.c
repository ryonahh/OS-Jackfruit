#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <signal.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024*1024)
#define SOCK_PATH "/tmp/mini.sock"

static char stack[STACK_SIZE];

typedef struct {
    char id[32];
    char rootfs[256];
    char cmd[256];
} req_t;

int child(void *arg)
{
    req_t *r = arg;

    sethostname(r->id, strlen(r->id));
    chroot(r->rootfs);
    chdir("/");
    mount("proc", "/proc", "proc", 0, NULL);

    char *argv[] = {"/bin/sh", "-c", r->cmd, NULL};
    execv("/bin/sh", argv);

    perror("exec");
    return 1;
}

void supervisor()
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);

    unlink(SOCK_PATH);
    bind(s, (struct sockaddr*)&addr, sizeof(addr));
    listen(s, 5);

    printf("Supervisor running...\n");

    while (1) {
        int c = accept(s, NULL, NULL);

        req_t r;
        read(c, &r, sizeof(r));

        pid_t pid = clone(child, stack+STACK_SIZE,
            CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
            &r);

        printf("Started %s PID=%d\n", r.id, pid);

        close(c);
    }
}

void start(char *id, char *root, char *cmd)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);

    connect(s, (struct sockaddr*)&addr, sizeof(addr));

    req_t r;
    strcpy(r.id, id);
    strcpy(r.rootfs, root);
    strcpy(r.cmd, cmd);

    write(s, &r, sizeof(r));
}

int main(int argc, char *argv[])
{
    if (!strcmp(argv[1], "supervisor"))
        supervisor();

    if (!strcmp(argv[1], "start"))
        start(argv[2], argv[3], argv[4]);

    return 0;
}
