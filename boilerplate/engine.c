// FULL ENGINE.C — REPLACE ENTIRE FILE

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor/monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"

typedef enum { CMD_START, CMD_RUN, CMD_PS, CMD_LOGS, CMD_STOP } cmd_t;
typedef enum { RUNNING, EXITED, KILLED } state_t;

typedef struct container {
    char id[32];
    pid_t pid;
    state_t state;
    int wait_fd;
    struct container *next;
} container_t;

typedef struct {
    cmd_t kind;
    char id[32];
    char rootfs[PATH_MAX];
    char cmd[256];
} request_t;

typedef struct {
    int status;
    char msg[256];
} response_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    container_t *list;
    pthread_mutex_t lock;
} ctx_t;

static ctx_t *g_ctx;
static char stack[STACK_SIZE];

int child_fn(void *arg)
{
    request_t *r = arg;

    sethostname(r->id, strlen(r->id));
    chroot(r->rootfs);
    chdir("/");
    mount("proc", "/proc", "proc", 0, NULL);

    char *argv[] = {"/bin/sh", "-c", r->cmd, NULL};
    execv("/bin/sh", argv);

    perror("exec");
    return 1;
}

void register_monitor(ctx_t *ctx, char *id, pid_t pid)
{
    if (ctx->monitor_fd < 0) return;

    struct monitor_request req = {0};
    req.pid = pid;
    req.soft_limit_bytes = 50UL << 20;
    req.hard_limit_bytes = 80UL << 20;
    strcpy(req.container_id, id);

    ioctl(ctx->monitor_fd, MONITOR_REGISTER, &req);
}

void unregister_monitor(ctx_t *ctx, char *id, pid_t pid)
{
    if (ctx->monitor_fd < 0) return;

    struct monitor_request req = {0};
    req.pid = pid;
    strcpy(req.container_id, id);

    ioctl(ctx->monitor_fd, MONITOR_UNREGISTER, &req);
}

void sigchld(int sig)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        container_t *c = g_ctx->list;

        while (c) {
            if (c->pid == pid) {
                unregister_monitor(g_ctx, c->id, pid);

                if (WIFEXITED(status))
                    c->state = EXITED;
                else
                    c->state = KILLED;

                if (c->wait_fd >= 0) {
                    response_t res = {0};
                    sprintf(res.msg, "Container %s finished", c->id);
                    send(c->wait_fd, &res, sizeof(res), 0);
                    close(c->wait_fd);
                }
                break;
            }
            c = c->next;
        }
    }
}

int supervisor()
{
    ctx_t ctx = {0};
    g_ctx = &ctx;

    pthread_mutex_init(&ctx.lock, NULL);

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(ctx.server_fd, 5);

    signal(SIGCHLD, sigchld);

    printf("Supervisor running...\n");

    while (1) {
        int fd = accept(ctx.server_fd, NULL, NULL);

        request_t req;
        recv(fd, &req, sizeof(req), 0);

        response_t res = {0};

        if (req.kind == CMD_START || req.kind == CMD_RUN) {
            request_t *child_req = malloc(sizeof(req));
            *child_req = req;

            pid_t pid = clone(child_fn, stack + STACK_SIZE,
                              CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                              child_req);

            container_t *c = malloc(sizeof(*c));
            strcpy(c->id, req.id);
            c->pid = pid;
            c->state = RUNNING;
            c->wait_fd = (req.kind == CMD_RUN) ? fd : -1;
            c->next = ctx.list;
            ctx.list = c;

            register_monitor(&ctx, req.id, pid);

            if (req.kind == CMD_START) {
                sprintf(res.msg, "Started %s pid=%d", req.id, pid);
                send(fd, &res, sizeof(res), 0);
                close(fd);
            }
        }

        else if (req.kind == CMD_PS) {
            char *p = res.msg;
            container_t *c = ctx.list;

            while (c) {
                p += sprintf(p, "%s %d %d\n", c->id, c->pid, c->state);
                c = c->next;
            }

            send(fd, &res, sizeof(res), 0);
            close(fd);
        }

        else if (req.kind == CMD_STOP) {
            container_t *c = ctx.list;

            while (c) {
                if (!strcmp(c->id, req.id)) {
                    kill(c->pid, SIGTERM);
                    sprintf(res.msg, "Stopped %s", req.id);
                    break;
                }
                c = c->next;
            }

            send(fd, &res, sizeof(res), 0);
            close(fd);
        }

        else if (req.kind == CMD_LOGS) {
            sprintf(res.msg, "logs/%s.log", req.id);
            send(fd, &res, sizeof(res), 0);
            close(fd);
        }
    }
}

int send_req(request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    send(fd, req, sizeof(*req), 0);

    response_t res;
    recv(fd, &res, sizeof(res), 0);

    printf("%s\n", res.msg);

    close(fd);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) return 1;

    if (!strcmp(argv[1], "supervisor"))
        return supervisor();

    request_t req = {0};

    if (!strcmp(argv[1], "start")) req.kind = CMD_START;
    else if (!strcmp(argv[1], "run")) req.kind = CMD_RUN;
    else if (!strcmp(argv[1], "ps")) req.kind = CMD_PS;
    else if (!strcmp(argv[1], "logs")) req.kind = CMD_LOGS;
    else if (!strcmp(argv[1], "stop")) req.kind = CMD_STOP;

    if (argc > 2) strcpy(req.id, argv[2]);
    if (argc > 3) strcpy(req.rootfs, argv[3]);
    if (argc > 4) strcpy(req.cmd, argv[4]);

    return send_req(&req);
}
