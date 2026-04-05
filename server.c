/* =============================================================================
 * server.c  —  Coordinator / Dispatcher  (Server)
 *
 * Responsibilities:
 *   1. Compile a user-supplied .c file with gcc.
 *   2. Query all known worker nodes for their CPU load (/proc/loadavg).
 *   3. Select the worker with the lowest 1-minute load average.
 *   4. Transfer the compiled binary over TCP using the custom protocol.
 *   5. Wait for the captured stdout/stderr and print it to the terminal.
 *
 * Usage:
 *   ./server
 *   (Then type tasks interactively, e.g., 'task_example.c')
 * ============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#include "protocol.h"

/* ── Types ─────────────────────────────────────────────────────────────── */

/** Holds the result of querying a single worker's load. */
typedef struct {
    char          ip[INET_ADDRSTRLEN];
    load_payload_t load;
    int            reachable;   /**< 1 = responded, 0 = timed out/error */
} worker_info_t;

/* ── Forward declarations ────────────────────────────────────────────────*/

static int  compile_source(const char *src_path, const char *out_path);
static int  connect_to_worker(const char *ip, int port);
static int  query_worker_load(const char *ip, worker_info_t *info);
static int  select_best_worker(worker_info_t *workers, int count);
static int  send_binary(int sockfd, const char *bin_path);
static int  receive_output(int sockfd);
static void set_socket_timeout(int sockfd, int seconds);
static void print_local_ip(void);
static int  make_listener(int port);
static void dispatch_task(const char *src_path, worker_info_t *workers, int wcount);

/* ═══════════════════════════════════════════════════════════════════════
 * main()
 * ═════════════════════════════════════════════════════════════════════ */
int main(void)
{
    worker_info_t workers[MAX_WORKERS];
    int           wcount = 0;

    printf("\n[SERVER] Starting Distributed Task Execution Server...\n");
    print_local_ip();

    int register_fd = make_listener(REGISTER_PORT);
    if (register_fd < 0) {
        fprintf(stderr, "[SERVER] Failed to bind register port.\n");
        return EXIT_FAILURE;
    }
    printf("[SERVER] Listening for worker registrations on port %d\n", REGISTER_PORT);
    printf("[SERVER] Ready! Commands:\n");
    printf("         <filename.c>  — dispatch task\n");
    printf("         workers       — list registered workers\n");
    printf("         quit          — exit\n\n");

    for (;;) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(register_fd, &readfds);

        printf("dte> ");
        fflush(stdout);

        int max_fd = (register_fd > STDIN_FILENO) ? register_fd : STDIN_FILENO;
        int activity = select(max_fd + 1, &readfds, NULL, NULL, NULL);

        if (activity < 0) {
            if (errno == EINTR) continue;
            perror("[SERVER] select");
            break;
        }

        /* ── 1. Handle incoming worker registrations ── */
        if (FD_ISSET(register_fd, &readfds)) {
            struct sockaddr_in caddr;
            socklen_t caddrlen = sizeof(caddr);
            int cfd = accept(register_fd, (struct sockaddr *)&caddr, &caddrlen);
            if (cfd >= 0) {
                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &caddr.sin_addr, client_ip, sizeof(client_ip));
                
                pkt_header_t h;
                if (recv_header(cfd, &h) == 0 && h.type == MSG_REGISTER) {
                    /* Check for duplicates */
                    int duplicate = 0;
                    for (int i = 0; i < wcount; i++) {
                        if (strcmp(workers[i].ip, client_ip) == 0) {
                            duplicate = 1;
                            break;
                        }
                    }
                    if (!duplicate) {
                        if (wcount < MAX_WORKERS) {
                            memset(&workers[wcount], 0, sizeof(worker_info_t));
                            strncpy(workers[wcount].ip, client_ip, INET_ADDRSTRLEN - 1);
                            wcount++;
                            printf("\n[SERVER] New worker registered: %s\n", client_ip);
                        } else {
                            printf("\n[SERVER] Max workers reached. Rejecting %s.\n", client_ip);
                        }
                        printf("dte> ");
                    }
                    send_header(cfd, MSG_REGISTER_ACK, FLAG_NONE, 0);
                }
                close(cfd);
                fflush(stdout);
            }
        }

        /* ── 2. Handle interactive stdin commands ── */
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            char line[256];
            if (!fgets(line, sizeof(line), stdin)) break;

            /* Strip newline */
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
            if (line[0] == '\0') continue;

            if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
                break;
            } else if (strcmp(line, "workers") == 0) {
                printf("[SERVER] Registered workers (%d/%d):\n", wcount, MAX_WORKERS);
                for (int i = 0; i < wcount; i++) {
                    printf("         - %s\n", workers[i].ip);
                }
            } else {
                dispatch_task(line, workers, wcount);
            }
        }
    }

    close(register_fd);
    return EXIT_SUCCESS;
}

/* ═══════════════════════════════════════════════════════════════════════
 * dispatch_task()
 * ═════════════════════════════════════════════════════════════════════ */
static void dispatch_task(const char *src_path, worker_info_t *workers, int wcount)
{
    if (wcount == 0) {
        fprintf(stderr, "[SERVER] Cannot dispatch task. No workers are registered yet.\n");
        return;
    }

    /* ── Validate source file ───────────────────────────────────────── */
    if (access(src_path, R_OK) != 0) {
        fprintf(stderr, "[SERVER] ERROR: Cannot read source file '%s': %s\n",
                src_path, strerror(errno));
        return;
    }

    /* ── Step 1: Compile the source file ────────────────────────────── */
    char bin_path[] = "/tmp/dte_compiled_XXXXXX";
    int  tmp_fd = mkstemp(bin_path);
    if (tmp_fd < 0) {
        perror("[SERVER] mkstemp");
        return;
    }
    close(tmp_fd);

    printf("[SERVER] Compiling '%s' → '%s' ...\n", src_path, bin_path);
    if (compile_source(src_path, bin_path) != 0) {
        fprintf(stderr, "[SERVER] Compilation failed. Aborting.\n");
        unlink(bin_path);
        return;
    }
    printf("[SERVER] Compilation successful.\n");

    /* ── Step 2: Query all workers for their load ───────────────────── */
    printf("[SERVER] Querying %d worker(s) for CPU load ...\n", wcount);

    for (int i = 0; i < wcount; i++) {
        printf("[SERVER]   %-15s : Querying... ", workers[i].ip);
        fflush(stdout);

        query_worker_load(workers[i].ip, &workers[i]);

        if (workers[i].reachable) {
            printf("SUCCESS (Load 1m=%.2f, 5m=%.2f, 15m=%.2f)\n",
                   workers[i].load.load_1min,
                   workers[i].load.load_5min,
                   workers[i].load.load_15min);
        } else {
            printf("FAILED (Unreachable)\n");
        }
    }

    /* ── Step 3: Select the best (lowest-load) worker ──────────────── */
    int chosen = select_best_worker(workers, wcount);
    if (chosen < 0) {
        fprintf(stderr, "[SERVER] No reachable workers found. Aborting.\n");
        unlink(bin_path);
        return;
    }
    printf("[SERVER] Selected worker: %-15s (Reason: Lowest 1-minute load = %.2f)\n",
           workers[chosen].ip, workers[chosen].load.load_1min);

    /* ── Step 4: Connect to selected worker and transfer binary ─────── */
    printf("[SERVER] Connecting to %s:%d ...\n", workers[chosen].ip, WORKER_PORT);
    int sockfd = connect_to_worker(workers[chosen].ip, WORKER_PORT);
    if (sockfd < 0) {
        fprintf(stderr, "[SERVER] Connection to worker failed.\n");
        unlink(bin_path);
        return;
    }
    set_socket_timeout(sockfd, NETWORK_TIMEOUT_SEC);

    printf("[SERVER] Sending binary ...\n");
    if (send_binary(sockfd, bin_path) != 0) {
        fprintf(stderr, "[SERVER] Binary transfer failed.\n");
        close(sockfd);
        unlink(bin_path);
        return;
    }
    printf("[SERVER] Binary sent. Waiting for execution output ...\n");

    /* ── Step 5: Receive and display output ─────────────────────────── */
    receive_output(sockfd);

    close(sockfd);
    unlink(bin_path);
}

/* ═══════════════════════════════════════════════════════════════════════
 * compile_source()
 *
 * Invokes gcc to compile @src_path and write the ELF binary to @out_path.
 * Returns 0 on success, -1 if gcc reports an error.
 *
 * Security note: In a production system you would use execvp() + fork()
 * with a carefully constructed argv[] instead of system().  system() is
 * used here per the project specification.
 * ═════════════════════════════════════════════════════════════════════ */
static int compile_source(const char *src_path, const char *out_path)
{
    /*
     * Build a gcc command that:
     *  -o out_path   : write binary here
     *  -Wall         : enable common warnings
     *  -O2           : optimise
     *  2>&1          : merge stderr into stdout so caller sees everything
     */
    char cmd[2048];
    int  ret = snprintf(cmd, sizeof(cmd),
                        "gcc -Wall -O2 -o '%s' '%s' 2>&1",
                        out_path, src_path);
    if (ret < 0 || (size_t)ret >= sizeof(cmd)) {
        fprintf(stderr, "[SERVER] compile_source: path too long\n");
        return -1;
    }

    printf("[SERVER] Running: %s\n", cmd);
    int status = system(cmd);

    if (status == -1) {
        perror("[SERVER] system()");
        return -1;
    }
    /* WEXITSTATUS(status)==0 means gcc succeeded */
    if (WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[SERVER] gcc exited with status %d\n",
                WEXITSTATUS(status));
        return -1;
    }

    /* Make the binary executable (mkstemp gives 0600) */
    if (chmod(out_path, 0755) != 0) {
        perror("[SERVER] chmod");
        return -1;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * connect_to_worker()
 *
 * Creates a TCP socket and connects to @ip:@port.
 * Returns the connected socket fd, or -1 on failure.
 * ═════════════════════════════════════════════════════════════════════ */
static int connect_to_worker(const char *ip, int port)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("[SERVER] socket");
        return -1;
    }

    /* Set a connection timeout using SO_SNDTIMEO */
    struct timeval tv = { .tv_sec = NETWORK_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "[SERVER] Invalid IP address: %s\n", ip);
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[SERVER] connect(%s:%d): %s\n", ip, port, strerror(errno));
        close(sockfd);
        return -1;
    }
    return sockfd;
}

/* ═══════════════════════════════════════════════════════════════════════
 * query_worker_load()
 *
 * Opens a short-lived connection to the worker's LOAD_QUERY_PORT,
 * sends MSG_QUERY_LOAD, and reads back a MSG_LOAD_RESPONSE.
 * Fills @info->load and sets @info->reachable accordingly.
 * ═════════════════════════════════════════════════════════════════════ */
static int query_worker_load(const char *ip, worker_info_t *info)
{
    info->reachable = 0;

    int sockfd = connect_to_worker(ip, LOAD_QUERY_PORT);
    if (sockfd < 0) return -1;

    set_socket_timeout(sockfd, 5);   /* short timeout for load probes */

    /* Send query header (no payload) */
    if (send_header(sockfd, MSG_QUERY_LOAD, FLAG_NONE, 0) != 0) {
        close(sockfd);
        return -1;
    }

    /* Receive response header */
    pkt_header_t h;
    if (recv_header(sockfd, &h) != 0) {
        close(sockfd);
        return -1;
    }

    if (h.type != MSG_LOAD_RESPONSE ||
        h.payload_len != sizeof(load_payload_t)) {
        fprintf(stderr, "[SERVER] query_worker_load(%s): unexpected response "
                "(type=0x%02x payload_len=%u)\n",
                ip, h.type, h.payload_len);
        close(sockfd);
        return -1;
    }

    if (read_all(sockfd, &info->load, sizeof(load_payload_t)) != 0) {
        close(sockfd);
        return -1;
    }

    info->load.running_procs = ntohs(info->load.running_procs);
    info->load.total_procs   = ntohs(info->load.total_procs);

    info->reachable = 1;
    close(sockfd);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * select_best_worker()
 *
 * Lowest-Load policy: iterate @workers[0..count-1] and return the index
 * of the reachable worker with the smallest 1-minute load average.
 * Returns -1 if no reachable worker exists.
 * ═════════════════════════════════════════════════════════════════════ */
static int select_best_worker(worker_info_t *workers, int count)
{
    int   best_idx  = -1;
    float best_load = 1e30f;

    for (int i = 0; i < count; i++) {
        if (!workers[i].reachable) continue;
        if (workers[i].load.load_1min < best_load) {
            best_load = workers[i].load.load_1min;
            best_idx  = i;
        }
    }
    return best_idx;
}

/* ═══════════════════════════════════════════════════════════════════════
 * send_binary()
 *
 * Protocol for sending the binary:
 *
 *   ┌──────────────────────────────────────────────────────────────────┐
 *   │  MSG_SEND_BINARY header  (payload_len = file size in bytes)      │
 *   ├──────────────────────────────────────────────────────────────────┤
 *   │  raw binary data in CHUNK_SIZE blocks                            │
 *   └──────────────────────────────────────────────────────────────────┘
 *
 * The worker knows exactly how many bytes to read from payload_len.
 * ═════════════════════════════════════════════════════════════════════ */
static int send_binary(int sockfd, const char *bin_path)
{
    /* Get file size */
    struct stat st;
    if (stat(bin_path, &st) != 0) {
        perror("[SERVER] stat binary");
        return -1;
    }
    uint32_t file_size = (uint32_t)st.st_size;

    /* Open binary */
    int fd = open(bin_path, O_RDONLY);
    if (fd < 0) {
        perror("[SERVER] open binary");
        return -1;
    }

    /* Send header announcing the payload size */
    if (send_header(sockfd, MSG_SEND_BINARY, FLAG_NONE, file_size) != 0) {
        perror("[SERVER] send_header");
        close(fd);
        return -1;
    }

    /* Stream binary in chunks */
    char   chunk[CHUNK_SIZE];
    size_t total_sent = 0;

    while (total_sent < file_size) {
        size_t to_read = CHUNK_SIZE;
        size_t remaining = file_size - total_sent;
        if (to_read > remaining) to_read = remaining;

        ssize_t n = read(fd, chunk, to_read);
        if (n <= 0) {
            fprintf(stderr, "[SERVER] read binary chunk: %s\n",
                    n < 0 ? strerror(errno) : "unexpected EOF");
            close(fd);
            return -1;
        }

        if (write_all(sockfd, chunk, (size_t)n) != 0) {
            perror("[SERVER] write_all chunk");
            close(fd);
            return -1;
        }
        total_sent += (size_t)n;
    }

    printf("[SERVER] Sent %zu byte(s).\n", total_sent);
    close(fd);

    /* Wait for ACK from worker before blocking on output */
    pkt_header_t ack;
    if (recv_header(sockfd, &ack) != 0 || ack.type != MSG_ACK) {
        fprintf(stderr, "[SERVER] Did not receive ACK after binary send.\n");
        return -1;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 * receive_output()
 *
 * Reads MSG_EXEC_RESULT from the worker and prints to stdout.
 * Also handles MSG_ERROR gracefully.
 * ═════════════════════════════════════════════════════════════════════ */
static int receive_output(int sockfd)
{
    pkt_header_t h;
    if (recv_header(sockfd, &h) != 0) {
        fprintf(stderr, "[SERVER] receive_output: connection closed unexpectedly\n");
        return -1;
    }

    if (h.type == MSG_ERROR) {
        /* Read the error payload */
        uint32_t elen = h.payload_len;
        if (elen > 512) elen = 512;
        char errbuf[513] = {0};
        read_all(sockfd, errbuf, elen);
        fprintf(stderr, "[SERVER] Worker returned error: %s\n", errbuf + 1);
        return -1;
    }

    if (h.type != MSG_EXEC_RESULT) {
        fprintf(stderr, "[SERVER] receive_output: unexpected message type 0x%02x\n",
                h.type);
        return -1;
    }

    /* Stream output to stdout */
    printf("\n"
           "╔══════════════════════════════════════════════════════════╗\n"
           "║              Remote Execution Output                    ║\n"
           "╚══════════════════════════════════════════════════════════╝\n");

    uint32_t remaining = h.payload_len;
    char     chunk[CHUNK_SIZE];

    while (remaining > 0) {
        uint32_t to_read = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;
        ssize_t  n = read(sockfd, chunk, to_read);
        if (n <= 0) break;
        fwrite(chunk, 1, (size_t)n, stdout);
        remaining -= (uint32_t)n;
    }

    printf("\n"
           "╚══════════════════════════════════════════════════════════╝\n");
    fflush(stdout);
    return 0;
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

/* ═══════════════════════════════════════════════════════════════════════
 * print_local_ip()
 * ═════════════════════════════════════════════════════════════════════ */
static void print_local_ip(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = inet_addr("8.8.8.8");
    serv.sin_port = htons(53);
    
    if (connect(s, (struct sockaddr *)&serv, sizeof(serv)) == 0) {
        struct sockaddr_in name;
        socklen_t namelen = sizeof(name);
        if (getsockname(s, (struct sockaddr *)&name, &namelen) == 0) {
            printf("[SERVER] Local IP address: %s\n", inet_ntoa(name.sin_addr));
        }
    }
    close(s);
}

/* ═══════════════════════════════════════════════════════════════════════
 * make_listener()
 * ═════════════════════════════════════════════════════════════════════ */
static int make_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("[SERVER] socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[SERVER] bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 16) < 0) {
        perror("[SERVER] listen");
        close(fd);
        return -1;
    }
    return fd;
}
