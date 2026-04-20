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
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16

/* ================= ENUMS ================= */

typedef enum {
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_EXITED,
    CONTAINER_KILLED
} container_state_t;

/* ================= STRUCTS ================= */

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t pid;
    container_state_t state;
    int exit_code;
    int wait_fd; // for RUN
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t len;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    int head, tail, count;
    int shutdown;
    pthread_mutex_t mtx;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} buffer_t;

typedef struct {
    command_kind_t kind;
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char cmd[256];
} request_t;

typedef struct {
    int status;
    char msg[256];
} response_t;

typedef struct {
    int server_fd;
    buffer_t buf;
    pthread_mutex_t lock;
    container_record_t *containers;
} ctx_t;

static ctx_t *g_ctx = NULL;
static char child_stack[STACK_SIZE];

/* ================= BUFFER ================= */

void buffer_init(buffer_t *b)
{
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->mtx, NULL);
    pthread_cond_init(&b->not_empty, NULL);
    pthread_cond_init(&b->not_full, NULL);
}

int buffer_push(buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mtx);

    while (b->count == LOG_BUFFER_CAPACITY && !b->shutdown)
        pthread_cond_wait(&b->not_full, &b->mtx);

    if (b->shutdown) {
        pthread_mutex_unlock(&b->mtx);
        return -1;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;

    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mtx);
    return 0;
}

int buffer_pop(buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mtx);

    while (b->count == 0 && !b->shutdown)
        pthread_cond_wait(&b->not_empty, &b->mtx);

    if (b->count == 0 && b->shutdown) {
        pthread_mutex_unlock(&b->mtx);
        return -1;
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;

    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mtx);
    return 0;
}

/* ================= LOGGING ================= */

void *logger(void *arg)
{
    buffer_t *b = arg;
    log_item_t item;

    mkdir(LOG_DIR, 0755);

    while (buffer_pop(b, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(path, O_CREAT | O_APPEND | O_WRONLY, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.len);
            close(fd);
        }
    }
    return NULL;
}

/* ================= PRODUCER ================= */

typedef struct {
    char id[CONTAINER_ID_LEN];
    int fd;
    buffer_t *buf;
} prod_args_t;

void *producer(void *arg)
{
    prod_args_t *p = arg;
    char buf[LOG_CHUNK_SIZE];
    ssize_t n;

    while ((n = read(p->fd, buf, sizeof(buf))) > 0) {
        log_item_t item = {0};
        strcpy(item.container_id, p->id);
        item.len = n;
        memcpy(item.data, buf, n);
        if (buffer_push(p->buf, &item) != 0) break;
    }

    close(p->fd);
    free(p);
    return NULL;
}

/* ================= CHILD ================= */

int child_fn(void *arg)
{
    request_t *r = arg;

    sethostname(r->id, strlen(r->id));
    chroot(r->rootfs);
    chdir("/");
    mount("proc", "/proc", "proc", 0, NULL);

    char *argv[] = {"/bin/sh", "-c", r->cmd, NULL};
    execv("/bin/sh", argv);

    return 1;
}

/* ================= SIGNAL ================= */

void sigchld_handler(int sig)
{
    (void)sig;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_ctx->lock);

        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->pid == pid) {
                if (WIFEXITED(status)) {
                    c->state = CONTAINER_EXITED;
                    c->exit_code = WEXITSTATUS(status);
                } else {
                    c->state = CONTAINER_KILLED;
                }

                if (c->wait_fd >= 0) {
                    response_t res = {0};
                    sprintf(res.msg, "Container %s finished", c->id);
                    send(c->wait_fd, &res, sizeof(res), 0);
                    close(c->wait_fd);
                    c->wait_fd = -1;
                }
                break;
            }
            c = c->next;
        }

        pthread_mutex_unlock(&g_ctx->lock);
    }
}

/* ================= SUPERVISOR ================= */

int run_supervisor()
{
    ctx_t ctx = {0};
    g_ctx = &ctx;

    pthread_mutex_init(&ctx.lock, NULL);
    buffer_init(&ctx.buf);

    signal(SIGCHLD, sigchld_handler);

    unlink(CONTROL_PATH);

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(ctx.server_fd, 5);

    pthread_t log_thread;
    pthread_create(&log_thread, NULL, logger, &ctx.buf);

    printf("Supervisor running...\n");

    while (1) {
        int fd = accept(ctx.server_fd, NULL, NULL);

        request_t req;
        recv(fd, &req, sizeof(req), 0);

        response_t res = {0};

        if (req.kind == CMD_START || req.kind == CMD_RUN) {

            int pipefd[2];
            pipe(pipefd);

            request_t *child_req = malloc(sizeof(req));
            *child_req = req;

            pid_t pid = clone(child_fn,
                              child_stack + STACK_SIZE,
                              CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                              child_req);

            container_record_t *c = malloc(sizeof(*c));
            strcpy(c->id, req.id);
            c->pid = pid;
            c->state = CONTAINER_RUNNING;
            c->wait_fd = (req.kind == CMD_RUN) ? fd : -1;
            c->next = ctx.containers;
            ctx.containers = c;

            if (req.kind == CMD_START) {
                sprintf(res.msg, "Started %s PID=%d", req.id, pid);
                send(fd, &res, sizeof(res), 0);
                close(fd);
            }

            prod_args_t *p = malloc(sizeof(*p));
            strcpy(p->id, req.id);
            p->fd = pipefd[0];
            p->buf = &ctx.buf;

            pthread_t t;
            pthread_create(&t, NULL, producer, p);
            pthread_detach(t);
        }

        else if (req.kind == CMD_PS) {
            char *ptr = res.msg;
            ptr += sprintf(ptr, "ID\tPID\tSTATE\n");

            container_record_t *c = ctx.containers;
            while (c) {
                ptr += sprintf(ptr, "%s\t%d\t%d\n", c->id, c->pid, c->state);
                c = c->next;
            }
            send(fd, &res, sizeof(res), 0);
            close(fd);
        }

        else if (req.kind == CMD_STOP) {
            container_record_t *c = ctx.containers;
            while (c) {
                if (strcmp(c->id, req.id) == 0) {
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
            sprintf(res.msg, "%s/%s.log", LOG_DIR, req.id);
            send(fd, &res, sizeof(res), 0);
            close(fd);
        }
    }
}

/* ================= CLIENT ================= */

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

/* ================= MAIN ================= */

int main(int argc, char *argv[])
{
    if (argc < 2) return 1;

    if (!strcmp(argv[1], "supervisor"))
        return run_supervisor();

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
