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
#include<string.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
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


extern supervisor_ctx_t *global_ctx;
supervisor_ctx_t *global_ctx = NULL;


static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}
//over
/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */


int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex); //acquire lock 
    //wait if the buffer is full 
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }
    //puts the thread to sleep and temporarily releases the lock so other threads (like a Consumer thread) can remove items and make space.

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;//add iteam into buffer
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY; //circular queue property
    buffer->count++;//increase the buffer size

    pthread_cond_signal(&buffer->not_empty);  //allow consumer to use buffer
    pthread_mutex_unlock(&buffer->mutex);//release lock

    return 0;
}

//over
/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex); //acquire lock 
    //wait if buffer is empty for producer to produce item
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }
    //add the consumer to the list and woke up when buffer size increasese
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    //consume
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--; //reduce buffer size

    pthread_cond_signal(&buffer->not_full); //
    pthread_mutex_unlock(&buffer->mutex);//release lock 

    return 0;
}

//over
/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    mkdir(LOG_DIR, 0755);

    while (1) {
        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0)
            break;

        char filepath[PATH_MAX];
        snprintf(filepath, sizeof(filepath), "%s/%s.log", LOG_DIR, item.container_id);

        FILE *fp = fopen(filepath, "a");
        if (!fp) {
            perror("fopen");
            continue;
        }

        fwrite(item.data, 1, item.length, fp);
        fclose(fp);
    }

    return NULL;
}

//over
/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */

int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;

    // 1. Set hostname (UTS namespace)
    sethostname(config->id, strlen(config->id));

    // 2. Change root filesystem
    if (chroot(config->rootfs) != 0) {
        perror("chroot failed");
        return 1;
    }

    // 3. Move into new root
    if (chdir("/") != 0) {
        perror("chdir failed");
        return 1;
    }

    // 4. Mount /proc
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount /proc failed");
        return 1;
    }

    // 5. Set nice value (optional)
    if (config->nice_value != 0) {
        nice(config->nice_value);
    }

    // 6. Execute command


    dup2(config->log_write_fd, STDOUT_FILENO);
    dup2(config->log_write_fd, STDERR_FILENO);

    //int devnull = open("/dev/null", O_RDONLY);
    //if (devnull >= 0) {
      //  dup2(devnull, STDIN_FILENO);
      //  close(devnull);
    //}

    close(config->log_write_fd);
    execl("/bin/sh", "sh", "-c", config->command, NULL);
    perror("execvp failed");  
    exit(1);
   
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}
//helper
typedef struct {
    int fd;
    supervisor_ctx_t *ctx;
    char container_id[CONTAINER_ID_LEN];
} pipe_args_t;
void *pipe_reader_thread(void *arg)
{
    pipe_args_t *args = (pipe_args_t *)arg;
    int fd = args->fd;
    supervisor_ctx_t *ctx = args->ctx;

    char buffer[LOG_CHUNK_SIZE];

    while (1) {
        ssize_t n = read(fd, buffer, sizeof(buffer));

        if (n < 0) {
            perror("read");
            break;
        }
        if (n == 0) {
            break; // container exited
        }

        log_item_t item;
        memset(&item, 0, sizeof(item));

        strncpy(item.container_id, args->container_id, CONTAINER_ID_LEN);
        item.length = n;
        memcpy(item.data, buffer, n);

        bounded_buffer_push(&ctx->log_buffer, &item);
    }

    close(fd);
    free(args);
    return NULL;
}


//over
/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */

void handle_sigchld(int sig)
{
    (void)sig;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        printf("🧹 Reaped child PID=%d\n", pid);

        if (global_ctx) {
            pthread_mutex_lock(&global_ctx->metadata_lock);

            container_record_t *cur = global_ctx->containers;
            while (cur) {
                if (cur->host_pid == pid) {
                    cur->state = CONTAINER_EXITED;

                    if (WIFEXITED(status))
                        cur->exit_code = WEXITSTATUS(status);
                    else if (WIFSIGNALED(status))
                        cur->exit_signal = WTERMSIG(status);

                    break;
                }
                cur = cur->next;
            }

            pthread_mutex_unlock(&global_ctx->metadata_lock);
        }
    }
}
static int run_supervisor(const char *rootfs)
{   (void)rootfs;
    supervisor_ctx_t ctx;
    int rc;
    global_ctx = &ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /*
     * TODO:
     *   1) open /dev/container_monitor
     *   2) create the control socket / FIFO / shared-memory channel
     *   3) install SIGCHLD / SIGINT / SIGTERM handling
     *   4) spawn the logger thread
     *   5) enter the supervisor event loop
     */
    //creatin new socket 
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    // Remove old socket file
    unlink(CONTROL_PATH);
    //-------------
    // 2. BIND
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    //3.Server listening confirmation
    if (listen(ctx.server_fd, 5) < 0) {
        perror("listen");
        return 1;
    }
    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    signal(SIGCHLD, handle_sigchld);
    printf("✅ Supervisor running... waiting for commands\n");
    while (1) {
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        control_request_t req;
        control_response_t resp;

        memset(&req, 0, sizeof(req));
        memset(&resp, 0, sizeof(resp));

        // Read request
        if (read(client_fd, &req, sizeof(req)) <= 0) {
            close(client_fd);
            continue;
        }

        printf("📩 Received command: %d (container=%s)\n", req.kind, req.container_id);

        // =========================
        // 5. HANDLE COMMAND
        // =========================
        switch (req.kind) {
        case CMD_RUN: {
                int pipefd[2];
                    if (pipe(pipefd) < 0) {
                        perror("pipe");
                        break;
                    }
                void *stack = malloc(STACK_SIZE);
                if (!stack) {
                    perror("malloc");
                    break;
                }
                child_config_t *config = malloc(sizeof(child_config_t));
                if (!config) {
                    perror("malloc");
                    free(stack);
                    break;
                }
               
                memset(config, 0, sizeof(*config));
                strncpy(config->id, req.container_id, sizeof(config->id) - 1);
                strncpy(config->rootfs, req.rootfs, sizeof(config->rootfs) - 1);
                strncpy(config->command, req.command, sizeof(config->command) - 1);
                config->nice_value = req.nice_value;
                config->log_write_fd = pipefd[1];
                int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;

                pid_t pid = clone(child_fn, (char *)stack + STACK_SIZE, flags, config);

                if (pid < 0) {
                    perror("clone");
                    free(stack);
                    free(config);
                    break;
                }
                close(pipefd[1]); // parent closes write end

                printf("🚀 Container started: %s (PID=%d)\n", req.container_id, pid);
                pthread_t reader_thread;
                container_record_t *rec = malloc(sizeof(container_record_t));
                memset(rec, 0, sizeof(*rec));

                strncpy(rec->id, req.container_id, CONTAINER_ID_LEN - 1);
                rec->host_pid = pid;
                rec->started_at = time(NULL);
                rec->state = CONTAINER_RUNNING;

                pthread_mutex_lock(&ctx.metadata_lock);
                rec->next = ctx.containers;
                ctx.containers = rec;
                pthread_mutex_unlock(&ctx.metadata_lock);

                pipe_args_t *args = malloc(sizeof(pipe_args_t));
                args->fd = pipefd[0];
                args->ctx = &ctx;
                strncpy(args->container_id, req.container_id, CONTAINER_ID_LEN);

                pthread_create(&reader_thread, NULL, pipe_reader_thread, args);
                                // Wait if RUN

                snprintf(resp.message, sizeof(resp.message),
                        "Container %s exited", req.container_id);

                break;
            }

        case CMD_START:
            snprintf(resp.message, sizeof(resp.message),
                     "START command received for container %s", req.container_id);
            break;

        case CMD_PS:
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *cur = ctx.containers;
            char buffer[2048];
            buffer[0] = '\0';
            strcat(buffer, "ID\tPID\tSTATE\tTIME\n");

            while (cur) {
                char line[128];
                snprintf(line, sizeof(line), "%s\t%d\t%s\t%ld\n",
                        cur->id,
                        cur->host_pid,
                        state_to_string(cur->state),
                        cur->started_at);

                strncat(buffer, line, sizeof(buffer) - strlen(buffer) - 1);
                cur = cur->next;
            }

            pthread_mutex_unlock(&ctx.metadata_lock);

            // 🔥 THIS IS THE MOST IMPORTANT LINE
            strncpy(resp.message, buffer, sizeof(resp.message) - 1);
            resp.message[sizeof(resp.message) - 1] = '\0';
            break;

        case CMD_STOP:
            snprintf(resp.message, sizeof(resp.message),
                     "STOP command received for %s", req.container_id);
            break;

        default:
            snprintf(resp.message, sizeof(resp.message),
                     "Unknown command");
            break;
        }

        resp.status = 0;

        // Send response
        ssize_t n = write(client_fd, &resp, sizeof(resp));
        if (n < 0) {
            perror("write failed");
        }
        close(client_fd);
    }
    
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 1;
}
//over
/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int sockfd;
    struct sockaddr_un addr;
    control_response_t resp;

    // 1. Create socket
    sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    // 2. Setup address
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    // 3. Connect to supervisor
    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is supervisor running?)");
        close(sockfd);
        return 1;
    }

    // 4. Send request
    if (write(sockfd, req, sizeof(*req)) != sizeof(*req)) {
        perror("write");
        close(sockfd);
        return 1;
    }

    // 5. Receive response
    if (read(sockfd, &resp, sizeof(resp)) > 0) {
        printf("%s\n", resp.message);
    } else {
        perror("read");
    }

    close(sockfd);
    return 0;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    printf("Expected states include: %s, %s, %s, %s, %s\n",
           state_to_string(CONTAINER_STARTING),
           state_to_string(CONTAINER_RUNNING),
           state_to_string(CONTAINER_STOPPED),
           state_to_string(CONTAINER_KILLED),
           state_to_string(CONTAINER_EXITED));
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
