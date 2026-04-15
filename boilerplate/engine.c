/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

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
#include <sys/resource.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 1024
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int stop_requested;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

typedef struct {
    int pipe_fd;
    char container_id[CONTAINER_ID_LEN];
    supervisor_ctx_t *ctx;
} producer_args_t;

supervisor_ctx_t *global_ctx = NULL;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog);
}

/* ==============================================================
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 * ============================================================== */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ==============================================================
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 * ============================================================== */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ==============================================================
 * TODO:
 * Implement the logging consumer thread.
 * ============================================================== */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    mkdir(LOG_DIR, 0777);

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        FILE *f = fopen(path, "a");
        if (f) {
            fwrite(item.data, 1, item.length, f);
            fclose(f);
        }
    }

    return NULL;
}

/* ==============================================================
 * TODO:
 * Implement the clone child entrypoint.
 * ============================================================== */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    setpriority(PRIO_PROCESS, 0, cfg->nice_value);

    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);

    if (chroot(cfg->rootfs) != 0 || chdir("/") != 0)
        return 1;

    mount("proc", "/proc", "proc", 0, NULL);

    char *argv_sh[] = {"/bin/sh", "-c", cfg->command, NULL};
    execv("/bin/sh", argv_sh);

    return 1;
}

/* ==============================================================
 * TODO:
 * Implement the long-running supervisor process.
 * ============================================================== */
static int run_supervisor(const char *rootfs)
{
    (void)rootfs;

    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    global_ctx = &ctx;

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);

    pthread_mutex_init(&ctx.metadata_lock, NULL);
    bounded_buffer_init(&ctx.log_buffer);

    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(ctx.server_fd, 5);

    printf("Supervisor started.\n");

    while (!ctx.should_stop) {
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0)
            continue;

        control_request_t req;
        control_response_t res = { .status = 0 };

        if (read(client_fd, &req, sizeof(req)) > 0) {

            if (req.kind == CMD_START) {
                int pipefd[2];
                pipe(pipefd);

                child_config_t *cfg = malloc(sizeof(child_config_t));
                strcpy(cfg->id, req.container_id);
                strcpy(cfg->rootfs, req.rootfs);
                strcpy(cfg->command, req.command);
                cfg->nice_value = req.nice_value;
                cfg->log_write_fd = pipefd[1];

                char *stack = malloc(STACK_SIZE);

                pid_t pid = clone(child_fn,
                                  stack + STACK_SIZE,
                                  CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD,
                                  cfg);

                close(pipefd[1]);

                producer_args_t *pargs = malloc(sizeof(producer_args_t));
                pargs->pipe_fd = pipefd[0];
                pargs->ctx = &ctx;
                strcpy(pargs->container_id, req.container_id);

                pthread_t t;
                pthread_create(&t, NULL, logging_thread, pargs);
                pthread_detach(t);

                register_with_monitor(ctx.monitor_fd,
                                      req.container_id,
                                      pid,
                                      req.soft_limit_bytes,
                                      req.hard_limit_bytes);

                snprintf(res.message, sizeof(res.message),
                         "Started container %s (PID %d)",
                         req.container_id, pid);
            }
        }

        write(client_fd, &res, sizeof(res));
        close(client_fd);
    }

    return 0;
}
