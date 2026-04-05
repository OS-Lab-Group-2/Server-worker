/* =============================================================================
 * task_example.c  —  Sample task to test the Distributed Execution System
 *
 * This file is compiled by Machine A and executed remotely on Machine B.
 * It demonstrates that stdout, stderr, and the process environment are all
 * correctly captured and returned.
 * ============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    /* ── Print to stdout ────────────────────────────────────────────── */
    printf("=== Distributed Task Execution System — Task Output ===\n\n");

    /* Hostname of the machine actually running this binary (Machine B) */
    char hostname[256] = {0};
    if (gethostname(hostname, sizeof(hostname) - 1) == 0)
        printf("Executing on host : %s\n", hostname);

    /* PID on the remote machine */
    printf("Remote PID        : %d\n", getpid());

    /* Wall clock time on the remote machine */
    time_t t = time(NULL);
    char   ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S %Z", localtime(&t));
    printf("Remote time       : %s\n", ts);

    /* Simple arithmetic to prove execution is real */
    printf("\nComputing first 10 Fibonacci numbers:\n");
    long a = 0, b = 1;
    printf("  F[0] = %ld\n", a);
    printf("  F[1] = %ld\n", b);
    for (int i = 2; i < 10; i++) {
        long c = a + b;
        printf("  F[%d] = %ld\n", i, c);
        a = b; b = c;
    }

    /* ── Print a line to stderr (also captured) ─────────────────────── */
    fprintf(stderr, "\n[STDERR] Task completed successfully on %s.\n",
            hostname);

    printf("\n=== Task finished ===\n");
    return EXIT_SUCCESS;
}
