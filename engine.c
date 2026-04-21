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
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define MAX_CONTAINERS 10

#define MAX_ID_LEN 32
#define MAX_PATH_LEN 256
#define MAX_CMD_LEN 256
#define MAX_STATE_LEN 24
#define RESPONSE_MSG_LEN 8192

#define CONTROL_SOCKET_PATH "/tmp/engine_supervisor.sock"
#define MONITOR_DEVICE_PATH "/dev/container_monitor"

#define DEFAULT_SOFT_MIB 40
#define DEFAULT_HARD_MIB 64
#define DEFAULT_NICE 0

#define LOG_BUFFER_CAPACITY 128
#define LOG_CHUNK_SIZE 512

typedef enum {
    REQ_START = 1,
    REQ_RUN,
    REQ_PS,
    REQ_LOGS,
    REQ_STOP
} request_type_t;

typedef struct {
    int type;
    char id[MAX_ID_LEN];
    char rootfs[MAX_PATH_LEN];
    char command[MAX_CMD_LEN];
    int soft_mib;
    int hard_mib;
    int nice_value;
} request_t;

typedef struct {
    int ok;
    int exit_code;
    char message[RESPONSE_MSG_LEN];
} response_t;

typedef struct {
    int used;
    char id[MAX_ID_LEN];
    pid_t pid;
    char state[MAX_STATE_LEN];
    char rootfs[MAX_PATH_LEN];
    char command[MAX_CMD_LEN];
    char log_file[MAX_PATH_LEN];
    int soft_mib;
    int hard_mib;
    int nice_value;
    int exit_code;
    int stop_requested;
    time_t start_time;
    void *child_stack;
    pthread_t producer_thread;
    pthread_cond_t state_cv;
} container_t;

typedef struct {
    int container_index;
    size_t len;
    char data[LOG_CHUNK_SIZE];
} log_entry_t;

typedef struct {
    log_entry_t entries[LOG_BUFFER_CAPACITY];
    int head;
    int tail;
    int count;
    int shutdown;
    pthread_mutex_t mutex;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} log_buffer_t;

typedef struct {
    int container_index;
    int read_fd;
} producer_arg_t;

typedef struct {
    char id[MAX_ID_LEN];
    char rootfs[MAX_PATH_LEN];
    char command[MAX_CMD_LEN];
    int nice_value;
    int write_fd;
} child_arg_t;

static container_t containers[MAX_CONTAINERS];
static pthread_mutex_t containers_mutex = PTHREAD_MUTEX_INITIALIZER;

static log_buffer_t g_log_buffer;
static pthread_t g_log_consumer_thread;
static pthread_t g_reaper_thread;

static volatile sig_atomic_t supervisor_running = 1;
static int server_socket_fd = -1;
static char supervisor_base_rootfs[MAX_PATH_LEN] = {0};

static void safe_strcpy(char *dst, const char *src, size_t dst_size) {
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static void format_time_string(time_t t, char *out, size_t out_size) {
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(out, out_size, "%Y-%m-%d %H:%M:%S", &tm_info);
}

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n == 0) return -1;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void init_response(response_t *resp) {
    memset(resp, 0, sizeof(*resp));
    resp->ok = 0;
    resp->exit_code = -1;
    resp->message[0] = '\0';
}

static int register_container_with_kernel(pid_t pid, int soft_mib, int hard_mib) {
    int fd = open(MONITOR_DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        perror("open /dev/container_monitor failed");
        return -1;
    }

    monitor_request_t req;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    req.soft_limit = soft_mib;
    req.hard_limit = hard_mib;

    if (ioctl(fd, REGISTER_CONTAINER, &req) != 0) {
        perror("REGISTER_CONTAINER ioctl failed");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int unregister_container_from_kernel(pid_t pid) {
    int fd = open(MONITOR_DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        perror("open /dev/container_monitor failed");
        return -1;
    }

    if (ioctl(fd, UNREGISTER_CONTAINER, &pid) != 0) {
        perror("UNREGISTER_CONTAINER ioctl failed");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static void set_response(response_t *resp, int ok, int exit_code, const char *fmt, ...) {
    va_list ap;
    resp->ok = ok;
    resp->exit_code = exit_code;
    va_start(ap, fmt);
    vsnprintf(resp->message, sizeof(resp->message), fmt, ap);
    va_end(ap);
}

static void log_buffer_init(log_buffer_t *buf) {
    memset(buf, 0, sizeof(*buf));
    pthread_mutex_init(&buf->mutex, NULL);
    pthread_cond_init(&buf->not_full, NULL);
    pthread_cond_init(&buf->not_empty, NULL);
}

static void log_buffer_shutdown(log_buffer_t *buf) {
    pthread_mutex_lock(&buf->mutex);
    buf->shutdown = 1;
    pthread_cond_broadcast(&buf->not_full);
    pthread_cond_broadcast(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
}

static int log_buffer_push(log_buffer_t *buf, const log_entry_t *entry) {
    pthread_mutex_lock(&buf->mutex);

    while (buf->count == LOG_BUFFER_CAPACITY && !buf->shutdown) {
        pthread_cond_wait(&buf->not_full, &buf->mutex);
    }

    if (buf->shutdown) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }

    buf->entries[buf->tail] = *entry;
    buf->tail = (buf->tail + 1) % LOG_BUFFER_CAPACITY;
    buf->count++;

    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

static int log_buffer_pop(log_buffer_t *buf, log_entry_t *entry_out) {
    pthread_mutex_lock(&buf->mutex);

    while (buf->count == 0 && !buf->shutdown) {
        pthread_cond_wait(&buf->not_empty, &buf->mutex);
    }

    if (buf->count == 0 && buf->shutdown) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }

    *entry_out = buf->entries[buf->head];
    buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
    buf->count--;

    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

static int is_live_state(const char *state) {
    return strcmp(state, "STARTING") == 0 || strcmp(state, "RUNNING") == 0;
}

static int find_free_container_slot_locked(void) {
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!containers[i].used) return i;
    }
    return -1;
}

static int find_container_by_id_locked(const char *id) {
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].used && strcmp(containers[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

static int rootfs_in_use_locked(const char *rootfs) {
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].used &&
            is_live_state(containers[i].state) &&
            strcmp(containers[i].rootfs, rootfs) == 0) {
            return 1;
        }
    }
    return 0;
}

static void init_containers(void) {
    memset(containers, 0, sizeof(containers));
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        pthread_cond_init(&containers[i].state_cv, NULL);
    }
}

static void *log_consumer_main(void *arg) {
    (void)arg;

    for (;;) {
        log_entry_t entry;
        if (log_buffer_pop(&g_log_buffer, &entry) != 0) {
            break;
        }

        char log_path[MAX_PATH_LEN] = {0};

        pthread_mutex_lock(&containers_mutex);
        if (entry.container_index >= 0 &&
            entry.container_index < MAX_CONTAINERS &&
            containers[entry.container_index].used) {
            safe_strcpy(log_path,
                        containers[entry.container_index].log_file,
                        sizeof(log_path));
        }
        pthread_mutex_unlock(&containers_mutex);

        if (log_path[0] == '\0') {
            continue;
        }

        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            continue;
        }

        (void)write(fd, entry.data, entry.len);
        close(fd);
    }

    return NULL;
}

static void *log_producer_main(void *arg) {
    producer_arg_t *parg = (producer_arg_t *)arg;
    int read_fd = parg->read_fd;
    int container_index = parg->container_index;
    free(parg);

    char buf[LOG_CHUNK_SIZE];
    for (;;) {
        ssize_t n = read(read_fd, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        log_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        entry.container_index = container_index;
        entry.len = (size_t)n;
        memcpy(entry.data, buf, (size_t)n);

        if (log_buffer_push(&g_log_buffer, &entry) != 0) {
            break;
        }
    }

    close(read_fd);
    return NULL;
}

static int child_main(void *arg) {
    child_arg_t *child = (child_arg_t *)arg;

    if (child->write_fd >= 0) {
        dup2(child->write_fd, STDOUT_FILENO);
        dup2(child->write_fd, STDERR_FILENO);
        close(child->write_fd);
    }

    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        close(devnull);
    }

    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    sethostname(child->id, strlen(child->id));
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);

    if (chroot(child->rootfs) != 0) {
        perror("chroot failed");
        _exit(1);
    }

    if (chdir("/") != 0) {
        perror("chdir failed");
        _exit(1);
    }

    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount /proc failed");
    }

    if (child->nice_value != 0) {
        setpriority(PRIO_PROCESS, 0, child->nice_value);
    }

    printf("Container %s started inside isolated rootfs\n", child->id);

    execl("/bin/sh", "/bin/sh", "-c", child->command, NULL);

    perror("exec failed");
    _exit(127);
}

static int start_container_internal(const request_t *req, int *index_out, response_t *resp) {
    int pipefd[2];
    void *stack = NULL;
    child_arg_t *child = NULL;
    pid_t pid;

    pthread_mutex_lock(&containers_mutex);

    if (find_container_by_id_locked(req->id) >= 0) {
        pthread_mutex_unlock(&containers_mutex);
        set_response(resp, 0, -1, "Container ID '%s' already exists", req->id);
        return -1;
    }

    if (rootfs_in_use_locked(req->rootfs)) {
        pthread_mutex_unlock(&containers_mutex);
        set_response(resp, 0, -1,
                     "Rootfs '%s' is already used by a live container", req->rootfs);
        return -1;
    }

    int idx = find_free_container_slot_locked();
    if (idx < 0) {
        pthread_mutex_unlock(&containers_mutex);
        set_response(resp, 0, -1, "Max containers reached");
        return -1;
    }

    containers[idx].used = 1;
    safe_strcpy(containers[idx].id, req->id, sizeof(containers[idx].id));
    safe_strcpy(containers[idx].rootfs, req->rootfs, sizeof(containers[idx].rootfs));
    safe_strcpy(containers[idx].command, req->command, sizeof(containers[idx].command));
    snprintf(containers[idx].log_file, sizeof(containers[idx].log_file), "logs/%s.log", req->id);
    safe_strcpy(containers[idx].state, "STARTING", sizeof(containers[idx].state));
    containers[idx].soft_mib = req->soft_mib;
    containers[idx].hard_mib = req->hard_mib;
    containers[idx].nice_value = req->nice_value;
    containers[idx].exit_code = -1;
    containers[idx].stop_requested = 0;
    containers[idx].start_time = time(NULL);
    containers[idx].pid = -1;
    containers[idx].child_stack = NULL;

    pthread_mutex_unlock(&containers_mutex);

    if (pipe(pipefd) != 0) {
        pthread_mutex_lock(&containers_mutex);
        containers[idx].used = 0;
        pthread_mutex_unlock(&containers_mutex);
        set_response(resp, 0, -1, "pipe failed: %s", strerror(errno));
        return -1;
    }

    stack = malloc(STACK_SIZE);
    if (!stack) {
        close(pipefd[0]);
        close(pipefd[1]);
        pthread_mutex_lock(&containers_mutex);
        containers[idx].used = 0;
        pthread_mutex_unlock(&containers_mutex);
        set_response(resp, 0, -1, "malloc failed");
        return -1;
    }

    child = malloc(sizeof(child_arg_t));
    if (!child) {
        free(stack);
        close(pipefd[0]);
        close(pipefd[1]);
        pthread_mutex_lock(&containers_mutex);
        containers[idx].used = 0;
        pthread_mutex_unlock(&containers_mutex);
        set_response(resp, 0, -1, "malloc failed");
        return -1;
    }

    memset(child, 0, sizeof(*child));
    safe_strcpy(child->id, req->id, sizeof(child->id));
    safe_strcpy(child->rootfs, req->rootfs, sizeof(child->rootfs));
    safe_strcpy(child->command, req->command, sizeof(child->command));
    child->nice_value = req->nice_value;
    child->write_fd = pipefd[1];

    pid = clone(child_main,
                (char *)stack + STACK_SIZE,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                child);

    if (pid < 0) {
        free(child);
        free(stack);
        close(pipefd[0]);
        close(pipefd[1]);
        pthread_mutex_lock(&containers_mutex);
        containers[idx].used = 0;
        pthread_mutex_unlock(&containers_mutex);
        set_response(resp, 0, -1, "clone failed: %s", strerror(errno));
        return -1;
    }

    free(child);
    close(pipefd[1]);

    producer_arg_t *parg = malloc(sizeof(producer_arg_t));
    if (!parg) {
        kill(pid, SIGKILL);
        close(pipefd[0]);
        free(stack);
        pthread_mutex_lock(&containers_mutex);
        containers[idx].used = 0;
        pthread_mutex_unlock(&containers_mutex);
        set_response(resp, 0, -1, "malloc failed");
        return -1;
    }

    parg->container_index = idx;
    parg->read_fd = pipefd[0];

    pthread_mutex_lock(&containers_mutex);
    containers[idx].pid = pid;
    containers[idx].child_stack = stack;
    safe_strcpy(containers[idx].state, "RUNNING", sizeof(containers[idx].state));
    pthread_mutex_unlock(&containers_mutex);

    pthread_create(&containers[idx].producer_thread, NULL, log_producer_main, parg);

    if (register_container_with_kernel(pid, req->soft_mib, req->hard_mib) != 0) {
        fprintf(stderr,
                "Warning: container %s started but kernel registration failed\n",
                req->id);
    }

    *index_out = idx;
    set_response(resp, 1, 0,
                 "Container %s started (PID %d)",
                 req->id, pid);
    return 0;
}

static void build_ps_output(char *out, size_t out_size) {
    size_t used = 0;
    used += snprintf(out + used, out_size - used,
                     "ID\tPID\tSTATE\tEXIT\tSOFT\tHARD\tNICE\tSTARTED\t\t\tLOG FILE\n");

    pthread_mutex_lock(&containers_mutex);

    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!containers[i].used) continue;

        char ts[32] = "-";
        if (containers[i].start_time != 0) {
            format_time_string(containers[i].start_time, ts, sizeof(ts));
        }

        used += snprintf(out + used, out_size - used,
                         "%s\t%d\t%s\t%d\t%d\t%d\t%d\t%s\t%s\n",
                         containers[i].id,
                         containers[i].pid,
                         containers[i].state,
                         containers[i].exit_code,
                         containers[i].soft_mib,
                         containers[i].hard_mib,
                         containers[i].nice_value,
                         ts,
                         containers[i].log_file);

        if (used >= out_size) break;
    }

    pthread_mutex_unlock(&containers_mutex);
}

static void build_logs_output(const char *id, char *out, size_t out_size, int *ok_out) {
    char log_path[MAX_PATH_LEN] = {0};

    pthread_mutex_lock(&containers_mutex);
    int idx = find_container_by_id_locked(id);
    if (idx >= 0) {
        safe_strcpy(log_path, containers[idx].log_file, sizeof(log_path));
    }
    pthread_mutex_unlock(&containers_mutex);

    if (log_path[0] == '\0') {
        snprintf(out, out_size, "No such container: %s\n", id);
        *ok_out = 0;
        return;
    }

    FILE *fp = fopen(log_path, "r");
    if (!fp) {
        snprintf(out, out_size, "Unable to open log file: %s\n", log_path);
        *ok_out = 0;
        return;
    }

    size_t used = 0;
    int c;
    while ((c = fgetc(fp)) != EOF && used + 1 < out_size) {
        out[used++] = (char)c;
    }
    out[used] = '\0';
    fclose(fp);

    if (used == 0) {
        snprintf(out, out_size, "(log file is empty)\n");
    }

    *ok_out = 1;
}

static int stop_container_by_id(const char *id, response_t *resp) {
    pthread_mutex_lock(&containers_mutex);

    int idx = find_container_by_id_locked(id);
    if (idx < 0) {
        pthread_mutex_unlock(&containers_mutex);
        set_response(resp, 0, -1, "No such container: %s", id);
        return -1;
    }

    if (!is_live_state(containers[idx].state)) {
        pthread_mutex_unlock(&containers_mutex);
        set_response(resp, 0, -1, "Container %s is not running", id);
        return -1;
    }

    containers[idx].stop_requested = 1;
    pid_t pid = containers[idx].pid;

    pthread_mutex_unlock(&containers_mutex);

    if (kill(pid, SIGTERM) != 0) {
        set_response(resp, 0, -1, "Failed to stop %s: %s", id, strerror(errno));
        return -1;
    }

    set_response(resp, 1, 0, "Stop requested for %s (PID %d)", id, pid);
    return 0;
}

static void *reaper_main(void *arg) {
    (void)arg;

    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, 0);

        if (pid < 0) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) {
                if (!supervisor_running) break;
                usleep(100000);
                continue;
            }
            continue;
        }

        pthread_mutex_lock(&containers_mutex);

        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (!containers[i].used || containers[i].pid != pid) continue;
                    
            if (WIFEXITED(status)) {
                if (containers[i].stop_requested) {
                    safe_strcpy(containers[i].state, "STOPPED", sizeof(containers[i].state));
                } else {
                    safe_strcpy(containers[i].state, "EXITED", sizeof(containers[i].state));
                }
                containers[i].exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                if (containers[i].stop_requested) {
                    safe_strcpy(containers[i].state, "STOPPED", sizeof(containers[i].state));
                } else if (WTERMSIG(status) == SIGKILL) {
                    safe_strcpy(containers[i].state, "KILLED", sizeof(containers[i].state));
                } else {
                    safe_strcpy(containers[i].state, "KILLED", sizeof(containers[i].state));
                }
                containers[i].exit_code = 128 + WTERMSIG(status);
            } else {
                safe_strcpy(containers[i].state, "STOPPED", sizeof(containers[i].state));
                containers[i].exit_code = -1;
            }

            pthread_cond_broadcast(&containers[i].state_cv);

            if (unregister_container_from_kernel(pid) != 0) {
                fprintf(stderr,
                        "Warning: failed to unregister PID %d from kernel monitor\n",
                        pid);
            }

            if (containers[i].child_stack) {
                free(containers[i].child_stack);
                containers[i].child_stack = NULL;
            }

            break;
        }
        pthread_mutex_unlock(&containers_mutex);
    }

    return NULL;
}

static void supervisor_signal_handler(int sig) {
    (void)sig;
    supervisor_running = 0;
    if (server_socket_fd >= 0) {
        close(server_socket_fd);
        server_socket_fd = -1;
    }
}

static void stop_all_live_containers(void) {
    pthread_mutex_lock(&containers_mutex);

    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!containers[i].used) continue;
        if (!is_live_state(containers[i].state)) continue;

        containers[i].stop_requested = 1;
        kill(containers[i].pid, SIGTERM);
    }

    pthread_mutex_unlock(&containers_mutex);
}

static void *client_handler_main(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);

    request_t req;
    response_t resp;
    init_response(&resp);

    if (recv_all(client_fd, &req, sizeof(req)) != 0) {
        close(client_fd);
        return NULL;
    }

    switch (req.type) {
        case REQ_START: {
            int idx = -1;
            start_container_internal(&req, &idx, &resp);
            break;
        }

        case REQ_RUN: {
            int idx = -1;
            if (start_container_internal(&req, &idx, &resp) != 0) {
                break;
            }

            pthread_mutex_lock(&containers_mutex);
            while (is_live_state(containers[idx].state)) {
                pthread_cond_wait(&containers[idx].state_cv, &containers_mutex);
            }

            int exit_code = containers[idx].exit_code;
            char final_state[MAX_STATE_LEN];
            safe_strcpy(final_state, containers[idx].state, sizeof(final_state));
            pthread_mutex_unlock(&containers_mutex);

            set_response(&resp, 1, exit_code,
                         "Container %s finished with state=%s exit=%d",
                         req.id, final_state, exit_code);
            break;
        }

        case REQ_PS: {
            build_ps_output(resp.message, sizeof(resp.message));
            resp.ok = 1;
            resp.exit_code = 0;
            break;
        }

        case REQ_LOGS: {
            int ok = 0;
            build_logs_output(req.id, resp.message, sizeof(resp.message), &ok);
            resp.ok = ok;
            resp.exit_code = ok ? 0 : -1;
            break;
        }

        case REQ_STOP: {
            stop_container_by_id(req.id, &resp);
            break;
        }

        default:
            set_response(&resp, 0, -1, "Unknown request");
            break;
    }

    (void)send_all(client_fd, &resp, sizeof(resp));
    close(client_fd);
    return NULL;
}

static int run_supervisor(const char *base_rootfs) {
    struct sockaddr_un addr;
    mkdir("logs", 0755);

    safe_strcpy(supervisor_base_rootfs, base_rootfs, sizeof(supervisor_base_rootfs));

    init_containers();
    log_buffer_init(&g_log_buffer);

    signal(SIGINT, supervisor_signal_handler);
    signal(SIGTERM, supervisor_signal_handler);

    pthread_create(&g_log_consumer_thread, NULL, log_consumer_main, NULL);
    pthread_create(&g_reaper_thread, NULL, reaper_main, NULL);

    unlink(CONTROL_SOCKET_PATH);

    server_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_socket_fd < 0) {
        perror("socket failed");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strcpy(addr.sun_path, CONTROL_SOCKET_PATH, sizeof(addr.sun_path));

    if (bind(server_socket_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind failed");
        close(server_socket_fd);
        unlink(CONTROL_SOCKET_PATH);
        return 1;
    }

    if (listen(server_socket_fd, 16) != 0) {
        perror("listen failed");
        close(server_socket_fd);
        unlink(CONTROL_SOCKET_PATH);
        return 1;
    }

    printf("Supervisor started. Base rootfs: %s\n", supervisor_base_rootfs);
    printf("Control socket: %s\n", CONTROL_SOCKET_PATH);

    while (supervisor_running) {
        int client_fd = accept(server_socket_fd, NULL, NULL);
        if (client_fd < 0) {
            if (!supervisor_running) break;
            if (errno == EINTR) continue;
            perror("accept failed");
            continue;
        }

        int *fd_ptr = malloc(sizeof(int));
        if (!fd_ptr) {
            close(client_fd);
            continue;
        }

        *fd_ptr = client_fd;
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_handler_main, fd_ptr) == 0) {
            pthread_detach(tid);
        } else {
            close(client_fd);
            free(fd_ptr);
        }
    }

    stop_all_live_containers();
    sleep(1);

    supervisor_running = 0;
    log_buffer_shutdown(&g_log_buffer);

    pthread_join(g_log_consumer_thread, NULL);
    pthread_join(g_reaper_thread, NULL);

    if (server_socket_fd >= 0) {
        close(server_socket_fd);
        server_socket_fd = -1;
    }
    unlink(CONTROL_SOCKET_PATH);

    printf("Supervisor stopped cleanly\n");
    return 0;
}

static int connect_to_supervisor(void) {
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strcpy(addr.sun_path, CONTROL_SOCKET_PATH, sizeof(addr.sun_path));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int send_request_to_supervisor(const request_t *req, response_t *resp) {
    int fd = connect_to_supervisor();
    if (fd < 0) {
        fprintf(stderr, "Could not connect to supervisor. Start it first:\n");
        fprintf(stderr, "  sudo ./engine supervisor ./rootfs-base\n");
        return 1;
    }

    if (send_all(fd, req, sizeof(*req)) != 0) {
        perror("send failed");
        close(fd);
        return 1;
    }

    if (recv_all(fd, resp, sizeof(*resp)) != 0) {
        perror("recv failed");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

static void init_request_defaults(request_t *req) {
    memset(req, 0, sizeof(*req));
    req->soft_mib = DEFAULT_SOFT_MIB;
    req->hard_mib = DEFAULT_HARD_MIB;
    req->nice_value = DEFAULT_NICE;
}

static int parse_optional_flags(int argc, char *argv[], int start_index, request_t *req) {
    for (int i = start_index; i < argc; i++) {
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (i + 1 >= argc) return -1;
            req->soft_mib = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--hard-mib") == 0) {
            if (i + 1 >= argc) return -1;
            req->hard_mib = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--nice") == 0) {
            if (i + 1 >= argc) return -1;
            req->nice_value = atoi(argv[++i]);
        } else {
            return -1;
        }
    }
    return 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor <base-rootfs>\n"
        "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s ps\n"
        "  %s logs <id>\n"
        "  %s stop <id>\n",
        prog, prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    request_t req;
    response_t resp;
    init_response(&resp);

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc != 3) {
            print_usage(argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    init_request_defaults(&req);

    if (strcmp(argv[1], "start") == 0 || strcmp(argv[1], "run") == 0) {
        if (argc < 5) {
            print_usage(argv[0]);
            return 1;
        }

        req.type = (strcmp(argv[1], "start") == 0) ? REQ_START : REQ_RUN;
        safe_strcpy(req.id, argv[2], sizeof(req.id));
        safe_strcpy(req.rootfs, argv[3], sizeof(req.rootfs));
        safe_strcpy(req.command, argv[4], sizeof(req.command));

        if (parse_optional_flags(argc, argv, 5, &req) != 0) {
            print_usage(argv[0]);
            return 1;
        }
    }
    else if (strcmp(argv[1], "ps") == 0) {
        if (argc != 2) {
            print_usage(argv[0]);
            return 1;
        }
        req.type = REQ_PS;
    }
    else if (strcmp(argv[1], "logs") == 0) {
        if (argc != 3) {
            print_usage(argv[0]);
            return 1;
        }
        req.type = REQ_LOGS;
        safe_strcpy(req.id, argv[2], sizeof(req.id));
    }
    else if (strcmp(argv[1], "stop") == 0) {
        if (argc != 3) {
            print_usage(argv[0]);
            return 1;
        }
        req.type = REQ_STOP;
        safe_strcpy(req.id, argv[2], sizeof(req.id));
    }
    else {
        print_usage(argv[0]);
        return 1;
    }

    if (send_request_to_supervisor(&req, &resp) != 0) {
        return 1;
    }

    if (resp.message[0] != '\0') {
        printf("%s\n", resp.message);
    }

    if (!resp.ok) {
        return 1;
    }

    if (req.type == REQ_RUN && resp.exit_code >= 0) {
        return resp.exit_code;
    }

    return 0;
}
