/* =============================================================================
 * protocol.h  —  Distributed Task Execution System
 * Shared header used by both Server (coordinator) and Worker.
 *
 * Wire format (every exchange begins with a pkt_header_t):
 *
 *   ┌──────────────┬──────────────┬──────────────┬──────────────────────────┐
 *   │  magic (4B)  │  type  (1B)  │  flags (1B)  │  payload_len (4B, BE)    │
 *   └──────────────┴──────────────┴──────────────┴──────────────────────────┘
 *   │                   payload  (payload_len bytes)                         │
 *   └────────────────────────────────────────────────────────────────────────┘
 *
 * All multi-byte integers are sent in network (big-endian) byte order.
 * ============================================================================*/

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <sys/types.h>

/* ── Compile-time constants ─────────────────────────────────────────────── */

/** Port on which Worker listens for binary-execution requests. */
#define WORKER_PORT          9100

/** Port on which Worker listens for load-query requests. */
#define LOAD_QUERY_PORT      9101

/** Port on which Server listens for worker auto-registration. */
#define REGISTER_PORT        9102

/** Magic number that opens every packet — catches garbage connections early. */
#define PROTO_MAGIC          0xDEADC0DE

/** Maximum payload we will accept in a single packet (256 MiB). */
#define MAX_PAYLOAD_SIZE     (256u * 1024u * 1024u)

/** How large each read() chunk is during file/output streaming (64 KiB). */
#define CHUNK_SIZE           (64u * 1024u)

/** connect() / recv() wall-clock timeout in seconds. */
#define NETWORK_TIMEOUT_SEC  30

/** Maximum number of worker nodes Server knows about. */
#define MAX_WORKERS          16

/** Path for temporary binary on Worker before execution. */
#define WORKER_TMP_BINARY    "/tmp/dte_task_exec"

/** Maximum captured output (stdout+stderr) returned to Server (8 MiB). */
#define MAX_OUTPUT_SIZE      (8u * 1024u * 1024u)

/* ── Message type codes ─────────────────────────────────────────────────── */

typedef enum {
    MSG_QUERY_LOAD    = 0x01,   /**< A→B  : "what is your current load?" */
    MSG_LOAD_RESPONSE = 0x02,   /**< B→A  : load_payload_t follows        */
    MSG_SEND_BINARY   = 0x03,   /**< A→B  : executable binary follows     */
    MSG_EXEC_RESULT   = 0x04,   /**< B→A  : captured output follows       */
    MSG_ACK           = 0x05,   /**< either direction: generic OK          */
    MSG_REGISTER      = 0x06,   /**< B→A  : auto-register on startup       */
    MSG_REGISTER_ACK  = 0x07,   /**< A→B  : registration successful        */
    MSG_ERROR         = 0xFF    /**< either direction: error_payload_t     */
} msg_type_t;

/* ── Flag bits (pkt_header_t.flags) ────────────────────────────────────── */

#define FLAG_NONE         0x00
#define FLAG_MORE_CHUNKS  0x01   /**< more data packets follow this one   */
#define FLAG_EXEC_STDERR  0x02   /**< payload contains stderr, not stdout  */

/* ── Wire structures ────────────────────────────────────────────────────── */

/**
 * pkt_header_t  —  10-byte fixed header that precedes every message.
 * Use send_header() / recv_header() helpers below rather than raw I/O.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;         /**< Must equal PROTO_MAGIC                  */
    uint8_t  type;          /**< One of msg_type_t                       */
    uint8_t  flags;         /**< Bitmask of FLAG_* values                */
    uint32_t payload_len;   /**< Byte length of payload that follows     */
} pkt_header_t;

/**
 * load_payload_t  —  payload sent in response to MSG_QUERY_LOAD.
 * All floats stored as IEEE-754 single precision, network byte order
 * is NOT applied to floats; both sides are Linux x86/ARM so endianness
 * is the same.  For a truly portable system you would encode as fixed-point.
 */
typedef struct __attribute__((packed)) {
    float    load_1min;     /**< /proc/loadavg 1-minute average          */
    float    load_5min;     /**< /proc/loadavg 5-minute average          */
    float    load_15min;    /**< /proc/loadavg 15-minute average         */
    uint16_t running_procs; /**< numerator  of "1/234" field             */
    uint16_t total_procs;   /**< denominator of "1/234" field            */
} load_payload_t;

/**
 * error_payload_t  —  human-readable error string (not NUL-terminated
 * on the wire; length is given by pkt_header_t.payload_len).
 * Allocate payload_len+1 bytes and NUL-terminate after receiving.
 */
typedef struct __attribute__((packed)) {
    uint8_t  error_code;    /**< application-level error code (see below)*/
    char     message[255];  /**< UTF-8 error message, may be shorter     */
} error_payload_t;

/* ── Application-level error codes ─────────────────────────────────────── */

typedef enum {
    ERR_OK              = 0x00,
    ERR_BAD_MAGIC       = 0x01,   /**< magic mismatch                    */
    ERR_UNKNOWN_TYPE    = 0x02,   /**< unrecognised msg_type              */
    ERR_PAYLOAD_TOO_BIG = 0x03,   /**< payload_len exceeds MAX_PAYLOAD   */
    ERR_EXEC_FAILED     = 0x04,   /**< fork/exec/wait failed             */
    ERR_WRITE_FAILED    = 0x05,   /**< could not write tmp binary        */
    ERR_INTERNAL        = 0xFF
} app_error_t;

/* ── Inline helper: fully reliable write ───────────────────────────────── */

/**
 * write_all()  —  keeps writing until all @len bytes are sent or an error
 * occurs.  Returns 0 on success, -1 on error (errno is preserved).
 */
static inline int write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n <= 0) return -1;
        p         += n;
        remaining -= (size_t)n;
    }
    return 0;
}

/**
 * read_all()  —  keeps reading until all @len bytes are received or the
 * connection closes.  Returns 0 on success, -1 on error/EOF.
 */
static inline int read_all(int fd, void *buf, size_t len)
{
    char *p = (char *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = read(fd, p, remaining);
        if (n <= 0) return -1;
        p         += n;
        remaining -= (size_t)n;
    }
    return 0;
}

/* ── Header send / receive helpers ─────────────────────────────────────── */

#include <arpa/inet.h>

/**
 * send_header()  —  serialises and transmits a pkt_header_t.
 * @payload_len is in host byte order; the function converts to network order.
 */
static inline int send_header(int fd, msg_type_t type,
                               uint8_t flags, uint32_t payload_len)
{
    pkt_header_t h;
    h.magic       = htonl(PROTO_MAGIC);
    h.type        = (uint8_t)type;
    h.flags       = flags;
    h.payload_len = htonl(payload_len);
    return write_all(fd, &h, sizeof(h));
}

/**
 * recv_header()  —  receives and validates a pkt_header_t.
 * On success returns 0 and fills *h with host-byte-order values.
 * Returns -1 on I/O error; -2 on protocol violation.
 */
static inline int recv_header(int fd, pkt_header_t *h)
{
    if (read_all(fd, h, sizeof(*h)) != 0) return -1;
    h->magic       = ntohl(h->magic);
    h->payload_len = ntohl(h->payload_len);
    if (h->magic != PROTO_MAGIC)          return -2;
    if (h->payload_len > MAX_PAYLOAD_SIZE) return -2;
    return 0;
}

#endif /* PROTOCOL_H */
