/* =============================================================================
 * worker.c  —  Worker / Daemon  (Worker)
 *
 * Responsibilities:
 *   1. Listen on LOAD_QUERY_PORT for MSG_QUERY_LOAD requests.
 *      → Read /proc/loadavg, reply with MSG_LOAD_RESPONSE.
 *   2. Listen on WORKER_PORT for MSG_SEND_BINARY requests.
 *      → Receive ELF binary, write to a temp file, execute it via
 *        fork()+execv(), capture stdout+stderr through a pipe,
 *        and send the captured output back as MSG_EXEC_RESULT.
 *   3. Both servers handle multiple simultaneous clients via fork().
 *
 * Compilation:
 *   gcc -Wall -O2 -o worker worker.c
 *
 * Run as a daemon:
 *   ./worker <server_ip>            (foreground, logs to stdout)
 *   ./worker <server_ip> --daemon   (daemonise, logs to /var/log/dte_worker.log)
 * ============================================================================*/

#define _POSIX_C_SOURCE 200809L
/* _DEFAULT_SOURCE enables daemon(), it is also set via -D flag in Makefile */
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "protocol.h"

/* ── Constants ──────────────────────────────────────────────────────────── */

#define LOG_PATH        "/var/log/dte_worker.log"
#define BACKLOG         16      /**< listen() queue depth                    */

/* ── Globals ────────────────────────────────────────────────────────────── */

static FILE *g_log = NULL;      /**< log file handle (or stdout)             */

/* ── Logging functions ──────────────────────────────────────────────────── */

#include <stdarg.h>

static void dte_log(const char *tag, const char *fmt, ...)
{
    FILE *out = g_log ? g_log : stdout;
    fprintf(out, "[WORKER]%s ", tag);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    if (g_log) fflush(g_log);
}

static void dte_log_err(const char *tag, const char *fmt, ...)
{
    FILE *out = g_log ? g_log : stderr;
    fprintf(out, "[WORKER][ERROR]%s ", tag);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fprintf(out, ": %s\n", strerror(errno));
    if (g_log) fflush(g_log);
}

#define LOG(fmt, ...)     dte_log("", fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) dte_log_err("", fmt, ##__VA_ARGS__)

/* ── Forward declarations ────────────────────────────────────────────────*/

static int  make_listener(int port);
static void run_load_server(int lfd);
static void run_exec_server(int lfd);
static void handle_load_client(int cfd, struct sockaddr_in *addr);
static void handle_exec_client(int cfd, struct sockaddr_in *addr);
static int  read_loadavg(load_payload_t *lp);
static int  receive_binary(int cfd, char *tmp_path, size_t path_len);
static int  execute_binary(const char *bin_path,
                            char **out_buf, size_t *out_len);
static void send_error(int cfd, app_error_t code, const char *msg);
static void reap_children(int sig);
static void set_socket_timeout(int sockfd, int seconds);
static void run_registration_loop(const char *server_ip);

/* ═══════════════════════════════════════════════════════════════════════
 * main()
 * ═════════════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <server_ip> [--daemon]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *server_ip = argv[1];
    int do_daemon = (argc >= 3 && strcmp(argv[2], "--daemon") == 0);

    if (do_daemon) {
        /* daemonise: double-fork, close stdio, redirect to log file */
        if (daemon(0, 0) != 0) {
            perror("daemon");
            return EXIT_FAILURE;
        }
        g_log = fopen(LOG_PATH, "a");
        if (!g_log) {
            /* Can't log — just keep stderr */
            g_log = stderr;
        }
    }

    LOG("Worker starting (pid=%d) connecting to Server at %s", getpid(), server_ip);

    /* Install SIGCHLD handler to reap zombie children automatically */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reap_children;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    /* Ignore SIGPIPE so that write() to a closed socket returns EPIPE
     * instead of killing the whole process.                             */
    signal(SIGPIPE, SIG_IGN);

    /* ── Create both listener sockets ──────────────────────────────── */
    int load_lfd = make_listener(LOAD_QUERY_PORT);
    int exec_lfd = make_listener(WORKER_PORT);

    if (load_lfd < 0 || exec_lfd < 0) {
        LOG("Failed to bind listener sockets. Exiting.");
        return EXIT_FAILURE;
    }

    LOG("Listening on port %d (load queries)", LOAD_QUERY_PORT);
    LOG("Listening on port %d (exec requests)", WORKER_PORT);

    /* ── Fork a child for the registration loop ───────────────────── */
    pid_t reg_child = fork();
    if (reg_child < 0) {
        LOG_ERR("fork for registration loop");
        return EXIT_FAILURE;
    }
    if (reg_child == 0) {
        close(load_lfd);
        close(exec_lfd);
        run_registration_loop(server_ip);
        return EXIT_SUCCESS;
    }

    /* ── Fork a child for the load-query server; parent runs exec ─── */
    pid_t child = fork();
    if (child < 0) {
        LOG_ERR("fork for load server");
        kill(reg_child, SIGTERM);
        return EXIT_FAILURE;
    }

    if (child == 0) {
        /* Child handles load queries */
        close(exec_lfd);
        run_load_server(load_lfd);
        return EXIT_SUCCESS;
    }

    /* Parent handles binary execution */
    close(load_lfd);
    run_exec_server(exec_lfd);

    /* Clean up children if parent exits */
    kill(child, SIGTERM);
    kill(reg_child, SIGTERM);
    waitpid(child, NULL, 0);
    waitpid(reg_child, NULL, 0);
    return EXIT_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════
 * run_registration_loop()
 * ═════════════════════════════════════════════════════════════════════ */
static void run_registration_loop(const char *server_ip)
{
    LOG("Registration daemon started for Server: %s", server_ip);
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(REGISTER_PORT);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
        LOG_ERR("Invalid Server IP: %s\n", server_ip);
        exit(EXIT_FAILURE);
    }

    while (1) {
        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd >= 0) {
            set_socket_timeout(sockfd, 5);
            if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                if (send_header(sockfd, MSG_REGISTER, FLAG_NONE, 0) == 0) {
                    pkt_header_t h;
                    if (recv_header(sockfd, &h) == 0 && h.type == MSG_REGISTER_ACK) {
                        /* Successfully registered */
                        /* Could log, but don't spam. */
                    }
                }
            }
            close(sockfd);
        }
        /* Sleep before trying again to keep the server's registry fresh */
        sleep(5);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * make_listener()
 *
 * Creates a TCP listening socket on @port with SO_REUSEADDR set.
 * Returns fd on success, -1 on error.
 * ═════════════════════════════════════════════════════════════════════ */
static int make_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { LOG_ERR("socket(%d)", port); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("bind(%d)", port);
        close(fd);
        return -1;
    }
    if (listen(fd, BACKLOG) < 0) {
        LOG_ERR("listen(%d)", port);
        close(fd);
        return -1;
    }
    return fd;
}

/* ═══════════════════════════════════════════════════════════════════════
 * run_load_server()  —  accept loop for LOAD_QUERY_PORT
 * ═════════════════════════════════════════════════════════════════════ */
static void run_load_server(int lfd)
{
    for (;;) {
        struct sockaddr_in caddr;
        socklen_t          caddrlen = sizeof(caddr);
        int cfd = accept(lfd, (struct sockaddr *)&caddr, &caddrlen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            LOG_ERR("accept load_server");
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            LOG_ERR("fork load_server");
            close(cfd);
            continue;
        }
        if (pid == 0) {
            /* Child */
            close(lfd);
            handle_load_client(cfd, &caddr);
            close(cfd);
            exit(EXIT_SUCCESS);
        }
        /* Parent */
        close(cfd);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * run_exec_server()  —  accept loop for WORKER_PORT
 * ═════════════════════════════════════════════════════════════════════ */
static void run_exec_server(int lfd)
{
    for (;;) {
        struct sockaddr_in caddr;
        socklen_t          caddrlen = sizeof(caddr);
        int cfd = accept(lfd, (struct sockaddr *)&caddr, &caddrlen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            LOG_ERR("accept exec_server");
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            LOG_ERR("fork exec_server");
            close(cfd);
            continue;
        }
        if (pid == 0) {
            close(lfd);
            handle_exec_client(cfd, &caddr);
            close(cfd);
            exit(EXIT_SUCCESS);
        }
        close(cfd);
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * handle_load_client()
 *
 * Reads /proc/loadavg and sends a MSG_LOAD_RESPONSE.
 * ═════════════════════════════════════════════════════════════════════ */
static void handle_load_client(int cfd, struct sockaddr_in *addr)
{
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, client_ip, sizeof(client_ip));
    LOG("Load query from %s", client_ip);

    set_socket_timeout(cfd, 10);

    pkt_header_t h;
    if (recv_header(cfd, &h) != 0 || h.type != MSG_QUERY_LOAD) {
        LOG("Bad load query from %s", client_ip);
        return;
    }

    load_payload_t lp;
    if (read_loadavg(&lp) != 0) {
        send_error(cfd, ERR_INTERNAL, "failed to read /proc/loadavg");
        return;
    }

    /* Convert multi-byte fields to network byte order */
    load_payload_t lp_net = lp;
    lp_net.running_procs = htons(lp.running_procs);
    lp_net.total_procs   = htons(lp.total_procs);

    if (send_header(cfd, MSG_LOAD_RESPONSE, FLAG_NONE,
                    (uint32_t)sizeof(load_payload_t)) != 0) return;
    write_all(cfd, &lp_net, sizeof(lp_net));
}

/* ═══════════════════════════════════════════════════════════════════════
 * handle_exec_client()
 *
 * Full execution flow:
 *   1. Receive the binary.
 *   2. ACK it.
 *   3. Execute it, capturing stdout+stderr.
 *   4. Send MSG_EXEC_RESULT with the captured output.
 * ═════════════════════════════════════════════════════════════════════ */
static void handle_exec_client(int cfd, struct sockaddr_in *addr)
{
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, client_ip, sizeof(client_ip));
    LOG("Exec request from %s", client_ip);

    set_socket_timeout(cfd, NETWORK_TIMEOUT_SEC);

    /* ── 1. Receive binary ──────────────────────────────────────────── */
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/dte_exec_%d", getpid());

    if (receive_binary(cfd, tmp_path, sizeof(tmp_path)) != 0) {
        LOG("Failed to receive binary from %s", client_ip);
        send_error(cfd, ERR_WRITE_FAILED, "binary receive failed");
        return;
    }
    LOG("Binary received → %s", tmp_path);

    /* ── 2. ACK the binary ──────────────────────────────────────────── */
    if (send_header(cfd, MSG_ACK, FLAG_NONE, 0) != 0) {
        unlink(tmp_path);
        return;
    }

    /* ── 3. Execute the binary ──────────────────────────────────────── */
    char  *out_buf = NULL;
    size_t out_len = 0;

    int exec_ret = execute_binary(tmp_path, &out_buf, &out_len);
    unlink(tmp_path);   /* clean up regardless */

    if (exec_ret != 0 && out_buf == NULL) {
        send_error(cfd, ERR_EXEC_FAILED, "fork/exec/wait failed");
        return;
    }

    /* ── 3.5. Echo task output on worker terminal ───────────────────── */
    LOG("\n"
        "========================= TASK OUTPUT =========================\n"
        "Client IP: %s\n", client_ip);

    if (out_buf && out_len > 0) {
        fwrite(out_buf, 1, out_len, g_log ? g_log : stdout);
        /* Ensure there's a newline if the output doesn't have one */
        if (out_buf[out_len - 1] != '\n') {
            fputc('\n', g_log ? g_log : stdout);
        }
    } else {
        LOG("(No output produced by the task)");
    }
    LOG("===============================================================");

    /* ── 4. Send captured output ────────────────────────────────────── */
    uint32_t send_len = (uint32_t)(out_len < MAX_OUTPUT_SIZE
                                   ? out_len : MAX_OUTPUT_SIZE);

    if (send_header(cfd, MSG_EXEC_RESULT, FLAG_NONE, send_len) != 0) {
        free(out_buf);
        return;
    }
    if (send_len > 0) {
        write_all(cfd, out_buf, send_len);
    }
    free(out_buf);

    LOG("Output sent (%u bytes) to %s", send_len, client_ip);
}

/* ═══════════════════════════════════════════════════════════════════════
 * read_loadavg()
 *
 * Parses /proc/loadavg and fills @lp.
 * Format:  0.00 0.01 0.05 1/234 5678
 *          ↑1m  ↑5m  ↑15m  ↑↑procs
 * ═════════════════════════════════════════════════════════════════════ */
static int read_loadavg(load_payload_t *lp)
{
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return -1;

    unsigned int running = 0, total = 0;
    int ret = fscanf(f, "%f %f %f %u/%u",
                     &lp->load_1min, &lp->load_5min, &lp->load_15min,
                     &running, &total);
    fclose(f);

    if (ret != 5) return -1;

    lp->running_procs = (uint16_t)running;
    lp->total_procs   = (uint16_t)total;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * receive_binary()
 *
 * Reads a MSG_SEND_BINARY header then streams payload_len bytes to a
 * temporary file at @tmp_path.  Makes the file executable (0700).
 * ═════════════════════════════════════════════════════════════════════ */
static int receive_binary(int cfd, char *tmp_path, size_t path_len)
{
    (void)path_len;

    pkt_header_t h;
    int rc = recv_header(cfd, &h);
    if (rc != 0) {
        LOG("recv_header failed (%d)", rc);
        return -1;
    }
    if (h.type != MSG_SEND_BINARY) {
        LOG("Expected MSG_SEND_BINARY, got 0x%02x", h.type);
        return -1;
    }
    if (h.payload_len == 0 || h.payload_len > MAX_PAYLOAD_SIZE) {
        LOG("Invalid payload_len: %u", h.payload_len);
        return -1;
    }

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    if (fd < 0) {
        LOG_ERR("open(%s)", tmp_path);
        return -1;
    }

    char    chunk[CHUNK_SIZE];
    uint32_t remaining = h.payload_len;

    while (remaining > 0) {
        uint32_t to_read = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;
        ssize_t  n = read(cfd, chunk, to_read);
        if (n <= 0) {
            LOG("Connection dropped while receiving binary");
            close(fd);
            unlink(tmp_path);
            return -1;
        }
        if (write_all(fd, chunk, (size_t)n) != 0) {
            LOG_ERR("write binary chunk");
            close(fd);
            unlink(tmp_path);
            return -1;
        }
        remaining -= (uint32_t)n;
    }

    close(fd);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * execute_binary()
 *
 * Fork-exec @bin_path, capturing stdout+stderr via a pipe.
 *
 * Pipe arrangement:
 *   Parent: pipe_fd[0] = read end
 *   Child : pipe_fd[1] = write end (dup2 → STDOUT_FILENO and STDERR_FILENO)
 *
 * The parent reads from pipe_fd[0] into a dynamically-grown buffer until
 * the child closes its write end (EOF).  Then it wait()s for the child.
 *
 * @out_buf  : set to a malloc'd buffer (caller must free).
 * @out_len  : set to the number of bytes in *out_buf.
 * Returns 0 on success (the child may still have exited non-zero),
 *         -1 if fork/exec/pipe failed.
 * ═════════════════════════════════════════════════════════════════════ */
static int execute_binary(const char *bin_path,
                           char **out_buf, size_t *out_len)
{
    *out_buf = NULL;
    *out_len = 0;

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        LOG_ERR("pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERR("fork(execute_binary)");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* ── Child process ─────────────────────────────────────────── */

        close(pipefd[0]);                       /* close read end       */

        /* Redirect stdout and stderr into the pipe */
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
            dup2(pipefd[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        close(pipefd[1]);

        /* Drop any inherited file descriptors (best-effort) */
        for (int i = 3; i < 1024; i++) close(i);

        /* Execute the binary.  argv[0] = program name by convention. */
        char *args[] = { (char *)bin_path, NULL };
        execv(bin_path, args);

        /* execv() only returns on failure */
        _exit(127);
    }

    /* ── Parent process ─────────────────────────────────────────────── */

    close(pipefd[1]);   /* close write end — child owns it */

    /* Dynamically grow a buffer to hold all output */
    size_t  cap  = 4096;
    size_t  used = 0;
    char   *buf  = malloc(cap);
    if (!buf) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        return -1;
    }

    char   tmp[4096];
    ssize_t n;
    while ((n = read(pipefd[0], tmp, sizeof(tmp))) > 0) {
        if (used + (size_t)n > MAX_OUTPUT_SIZE) break;  /* cap output */

        if (used + (size_t)n > cap) {
            while (cap < used + (size_t)n) cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
        }
        memcpy(buf + used, tmp, (size_t)n);
        used += (size_t)n;
    }
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        LOG("Child exited with status %d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        LOG("Child killed by signal %d", WTERMSIG(status));
    }

    *out_buf = buf;
    *out_len = used;
    return (buf != NULL) ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════════════
 * send_error()
 * ═════════════════════════════════════════════════════════════════════ */
static void send_error(int cfd, app_error_t code, const char *msg)
{
    size_t mlen    = strlen(msg);
    size_t payload = 1 + (mlen < 254 ? mlen : 254);

    char buf[256];
    buf[0] = (char)code;
    memcpy(buf + 1, msg, payload - 1);
    buf[payload] = '\0';

    send_header(cfd, MSG_ERROR, FLAG_NONE, (uint32_t)payload);
    write_all(cfd, buf, payload);
}

/* ═══════════════════════════════════════════════════════════════════════
 * reap_children()  —  SIGCHLD handler
 * ═════════════════════════════════════════════════════════════════════ */
static void reap_children(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

/* ═══════════════════════════════════════════════════════════════════════
 * set_socket_timeout()
 * ═════════════════════════════════════════════════════════════════════ */
static void set_socket_timeout(int sockfd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
