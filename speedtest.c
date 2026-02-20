/*
 * speedtest.c — multi-threaded bandwidth test via Cloudflare
 *
 * Compile:
 *   Linux:  gcc -O2 -pthread -o speedtest speedtest.c
 *   macOS:  clang -O2 -pthread -o speedtest speedtest.c
 *
 * Run:
 *   ./speedtest                          # auto (4*CPUs connections, 15s)
 *   ./speedtest -n 256 -t 20            # 256 connections, 20 seconds
 *   ./speedtest -n 512 -b 500000000     # 512 conns, 500 MB per request
 *
 * Kernel tuning for 10 Gbps+ (Linux):
 *   sysctl -w net.core.rmem_max=67108864
 *   sysctl -w net.core.wmem_max=67108864
 *   sysctl -w net.ipv4.tcp_rmem="4096 1048576 67108864"
 *   sysctl -w net.core.netdev_max_backlog=50000
 *   sysctl -w net.ipv4.tcp_max_syn_backlog=30000
 *   sysctl -w net.core.somaxconn=65535
 *   ulimit -n 65535
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>

#ifdef __linux__
#include <sched.h>
#endif

/* ═══════════════════════════════════════════════
 *  Configuration
 * ═══════════════════════════════════════════════ */

#define HOST          "speed.cloudflare.com"
#define PORT          "80"
#define RDBUF_SZ      (512 * 1024)         /* 512 KiB read buffer/thread   */
#define SOBUF_SZ      (8  * 1024 * 1024)   /* SO_RCVBUF hint: 8 MiB        */
#define FLUSH_EVERY   (4  * 1024 * 1024)   /* flush atomic every 4 MiB     */
#define HDR_MAX       4096                 /* max HTTP header size          */
#define STACK_SZ      (256 * 1024)         /* thread stack: 256 KiB        */
#define DEF_BYTES     100000000L           /* 100 MB per request            */
#define DEF_DUR       15                   /* test duration: 15 seconds     */

/* ═══════════════════════════════════════════════
 *  Shared atomic counters
 * ═══════════════════════════════════════════════ */

static _Atomic long long g_bytes = 0;     /* total body bytes received     */
static _Atomic int       g_run   = 1;     /* 0 → workers stop             */
static _Atomic int       g_act   = 0;     /* active connections now        */
static _Atomic long long g_ok    = 0;     /* completed downloads           */
static _Atomic long long g_err   = 0;     /* failed attempts               */

/* ═══════════════════════════════════════════════
 *  Cached DNS result
 * ═══════════════════════════════════════════════ */

#define MAX_ADDRS 8
static struct sockaddr_storage g_addrs[MAX_ADDRS];
static socklen_t               g_addrlens[MAX_ADDRS];
static int                     g_naddrs = 0;
static char                    g_ip_str[INET6_ADDRSTRLEN];

/* ═══════════════════════════════════════════════
 *  Helpers
 * ═══════════════════════════════════════════════ */

static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

static void on_signal(int s)
{
    (void)s;
    atomic_store_explicit(&g_run, 0, memory_order_relaxed);
}

static void fmt_speed(double mbps, char *buf, size_t sz)
{
    if      (mbps >= 1000.0) snprintf(buf, sz, "%7.2f Gbps", mbps / 1000.0);
    else                     snprintf(buf, sz, "%7.2f Mbps", mbps);
}

/* ═══════════════════════════════════════════════
 *  DNS — resolve once, reuse for every connect
 * ═══════════════════════════════════════════════ */

static int dns_resolve(void)
{
    struct addrinfo hints = {0}, *res, *rp;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(HOST, PORT, &hints, &res);
    if (rc) {
        fprintf(stderr, "DNS error for %s: %s\n", HOST, gai_strerror(rc));
        return -1;
    }

    /* store all returned addresses for round-robin */
    g_naddrs = 0;
    for (rp = res; rp && g_naddrs < MAX_ADDRS; rp = rp->ai_next) {
        memcpy(&g_addrs[g_naddrs], rp->ai_addr, rp->ai_addrlen);
        g_addrlens[g_naddrs] = rp->ai_addrlen;
        g_naddrs++;
    }

    /* pretty-print first address */
    void *addr = (res->ai_family == AF_INET)
        ? (void *)&((struct sockaddr_in  *)res->ai_addr)->sin_addr
        : (void *)&((struct sockaddr_in6 *)res->ai_addr)->sin6_addr;
    inet_ntop(res->ai_family, addr, g_ip_str, sizeof g_ip_str);

    freeaddrinfo(res);
    return g_naddrs > 0 ? 0 : -1;
}

/* ═══════════════════════════════════════════════
 *  TCP connect with tuned socket
 * ═══════════════════════════════════════════════ */

static int tcp_connect(int addr_idx)
{
    int i  = addr_idx % g_naddrs;
    int fd = socket(g_addrs[i].ss_family, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    if (connect(fd, (struct sockaddr *)&g_addrs[i], g_addrlens[i]) < 0) {
        close(fd);
        return -1;
    }

    int val;
    val = SOBUF_SZ;  setsockopt(fd, SOL_SOCKET,  SO_RCVBUF,   &val, sizeof val);
    val = 1;         setsockopt(fd, IPPROTO_TCP,  TCP_NODELAY, &val, sizeof val);
#ifdef TCP_QUICKACK
    val = 1;         setsockopt(fd, IPPROTO_TCP,  TCP_QUICKACK,&val, sizeof val);
#endif
    return fd;
}

/* ═══════════════════════════════════════════════
 *  Full write helper
 * ═══════════════════════════════════════════════ */

static int write_all(int fd, const void *data, int len)
{
    const char *p = data;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) return -1;
        p   += n;
        len -= (int)n;
    }
    return 0;
}

/* ═══════════════════════════════════════════════
 *  Worker thread
 * ═══════════════════════════════════════════════ */

typedef struct {
    int  id;
    long req_bytes;
} worker_arg_t;

static void *worker_fn(void *arg)
{
    worker_arg_t *wa = arg;

    /* pin to CPU core (Linux only) */
#ifdef __linux__
    {
        cpu_set_t cs;
        CPU_ZERO(&cs);
        CPU_SET(wa->id % cpu_count(), &cs);
        pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
    }
#endif

    /* pre-format HTTP request */
    char req[512];
    int  rlen = snprintf(req, sizeof req,
            "GET /__down?bytes=%ld HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "\r\n",
            wa->req_bytes, HOST);

    /* per-thread read buffer */
    char *buf = malloc(RDBUF_SZ);
    if (!buf) return NULL;

    int conn_num = 0;

    /* ── main loop: connect → download → repeat ── */
    while (atomic_load_explicit(&g_run, memory_order_relaxed)) {

        /* connect */
        int fd = tcp_connect(wa->id + conn_num++);
        if (fd < 0) {
            atomic_fetch_add_explicit(&g_err, 1, memory_order_relaxed);
            usleep(50000);          /* 50 ms backoff */
            continue;
        }
        atomic_fetch_add_explicit(&g_act, 1, memory_order_relaxed);

        /* send HTTP request */
        if (write_all(fd, req, rlen) < 0)
            goto fail;

        /* ── Phase 1: read & skip HTTP headers ── */
        char hdr[HDR_MAX];
        int  hdr_len  = 0;
        int  body_off = -1;
        long long batch = 0;

        while (body_off < 0) {
            if (!atomic_load_explicit(&g_run, memory_order_relaxed))
                goto done;

            int space = HDR_MAX - hdr_len - 1;
            if (space <= 0) goto fail;          /* headers impossibly large */

            ssize_t n = read(fd, hdr + hdr_len, space);
            if (n <= 0) goto done;

            hdr_len    += (int)n;
            hdr[hdr_len] = '\0';

            char *sep = strstr(hdr, "\r\n\r\n");
            if (sep) {
                body_off = (int)(sep + 4 - hdr);

                /* check HTTP status */
                int status = 0;
                sscanf(hdr, "HTTP/%*s %d", &status);
                if (status != 200) goto fail;

                /* body bytes already in header buffer */
                batch = hdr_len - body_off;
            }
        }

        /* ── Phase 2: read body as fast as possible ── */
        for (;;) {
            if (!atomic_load_explicit(&g_run, memory_order_relaxed))
                break;

            ssize_t n = read(fd, buf, RDBUF_SZ);
            if (n <= 0) break;

            batch += n;
            if (batch >= FLUSH_EVERY) {
                atomic_fetch_add_explicit(&g_bytes, batch,
                                          memory_order_relaxed);
                batch = 0;
            }
        }

        /* flush remaining batch */
        if (batch > 0)
            atomic_fetch_add_explicit(&g_bytes, batch,
                                      memory_order_relaxed);

        close(fd);
        atomic_fetch_sub_explicit(&g_act, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_ok,  1, memory_order_relaxed);
        continue;

    fail:
        close(fd);
        atomic_fetch_sub_explicit(&g_act, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_err, 1, memory_order_relaxed);
        usleep(20000);
        continue;

    done:
        if (batch > 0)
            atomic_fetch_add_explicit(&g_bytes, batch,
                                      memory_order_relaxed);
        close(fd);
        atomic_fetch_sub_explicit(&g_act, 1, memory_order_relaxed);
    }

    free(buf);
    return NULL;
}

/* ═══════════════════════════════════════════════
 *  main
 * ═══════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    long req_bytes = DEF_BYTES;
    int  duration  = DEF_DUR;
    int  nconn     = 0;                    /* 0 = auto */

    /* ── parse args ── */
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-n") && i+1 < argc)
            nconn    = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i+1 < argc)
            duration = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i+1 < argc)
            req_bytes = atol(argv[++i]);
        else {
            fprintf(stderr,
                "Usage: %s [-n connections] [-t seconds] [-b bytes_per_req]\n\n"
                "  -n  Parallel HTTP connections (default: auto = CPU*4, min 64)\n"
                "  -t  Test duration in seconds  (default: %d)\n"
                "  -b  Bytes per HTTP request     (default: %ld = %.0f MB)\n\n"
                "  For 10+ Gbps: use -n 256 or higher\n"
                "  For 40+ Gbps: use -n 512, -b 500000000, tune kernel\n\n",
                argv[0], DEF_DUR, DEF_BYTES, DEF_BYTES / 1e6);
            return 1;
        }
    }

    int ncpu = cpu_count();
    if (nconn <= 0) nconn = ncpu * 4;
    if (nconn < 64) nconn = 64;           /* minimum for high-bw links */
    if (duration  <= 0) duration  = DEF_DUR;
    if (req_bytes <= 0) req_bytes = DEF_BYTES;

    /* signals */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* ── banner ── */
    printf("\n");
    printf("  ==================================================\n");
    printf("   Multi-Threaded Bandwidth Test  %s:%s\n", HOST, PORT);
    printf("  ==================================================\n");
    printf("   CPU cores       : %d\n",    ncpu);
    printf("   Connections     : %d\n",    nconn);
    printf("   Per-request     : %.0f MB\n", req_bytes / 1e6);
    printf("   Duration        : %d s\n",  duration);
    printf("   Read buffer     : %d KiB/thread\n", RDBUF_SZ / 1024);
    printf("   SO_RCVBUF       : %d MiB\n", SOBUF_SZ / (1024*1024));
    printf("   Thread memory   : ~%.0f MB total\n",
           nconn * (RDBUF_SZ + STACK_SZ) / 1e6);

    /* ── DNS ── */
    printf("   Resolving DNS   : ");
    fflush(stdout);
    if (dns_resolve() < 0) return 1;
    printf("%s (%d addresses)\n\n", g_ip_str, g_naddrs);

    /* ── spawn worker threads ── */
    pthread_t    *threads = calloc(nconn, sizeof *threads);
    worker_arg_t *args    = calloc(nconn, sizeof *args);
    if (!threads || !args) { perror("calloc"); return 1; }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, STACK_SZ);

    double t0 = mono();

    for (int i = 0; i < nconn; i++) {
        args[i].id        = i;
        args[i].req_bytes = req_bytes;
        if (pthread_create(&threads[i], &attr, worker_fn, &args[i]) != 0) {
            fprintf(stderr, "pthread_create #%d failed: %s\n",
                    i, strerror(errno));
            nconn = i;
            break;
        }
    }
    pthread_attr_destroy(&attr);

    if (nconn == 0) {
        fprintf(stderr, "No threads created.\n");
        return 1;
    }

    /* ── monitor loop: print stats every second ── */
    printf("    Time | Conns |    Speed (1s)   |   Avg Speed     "
           "| Downloaded |  DL#\n");
    printf("   ------|-------|-----------------|---------------"
           "--|------------|------\n");

    long long prev_bytes = 0;
    double    prev_time  = t0;
    double    peak_mbps  = 0;

    while (atomic_load_explicit(&g_run, memory_order_relaxed)) {
        usleep(1000000);                   /* 1 second */

        double    now   = mono();
        double    total = now - t0;
        long long cur   = atomic_load_explicit(&g_bytes, memory_order_relaxed);
        int       act   = atomic_load_explicit(&g_act,   memory_order_relaxed);
        long long ok    = atomic_load_explicit(&g_ok,    memory_order_relaxed);
        long long err   = atomic_load_explicit(&g_err,   memory_order_relaxed);

        /* interval (1s) speed */
        double dt   = now - prev_time;
        double inst = dt > 0 ? ((cur - prev_bytes) * 8.0 / 1e6) / dt : 0;

        /* cumulative average speed */
        double avg  = total > 0 ? (cur * 8.0 / 1e6) / total : 0;

        if (inst > peak_mbps) peak_mbps = inst;

        char s_inst[32], s_avg[32];
        fmt_speed(inst, s_inst, sizeof s_inst);
        fmt_speed(avg,  s_avg,  sizeof s_avg);

        printf("   %4.0fs  |  %4d | %s  |  %s  | %7.2f GB  | %lld",
               total, act, s_inst, s_avg, cur / 1e9, (long long)ok);
        if (err > 0)
            printf("  (err:%lld)", (long long)err);
        printf("\n");
        fflush(stdout);

        prev_bytes = cur;
        prev_time  = now;

        if (total >= duration)
            atomic_store_explicit(&g_run, 0, memory_order_relaxed);
    }

    /* ── join all threads ── */
    printf("\n   Stopping threads...");
    fflush(stdout);

    for (int i = 0; i < nconn; i++)
        pthread_join(threads[i], NULL);

    printf(" done.\n");

    /* ── final results ── */
    double    T    = mono() - t0;
    long long B    = atomic_load(&g_bytes);
    long long OK   = atomic_load(&g_ok);
    long long ERR  = atomic_load(&g_err);
    double    avg  = T > 0 ? (B * 8.0 / 1e6) / T : 0;

    char s_avg[32], s_peak[32];
    fmt_speed(avg,      s_avg,  sizeof s_avg);
    fmt_speed(peak_mbps, s_peak, sizeof s_peak);

    printf("\n");
    printf("   +============================================+\n");
    printf("   |              R E S U L T S                 |\n");
    printf("   +============================================+\n");
    printf("   |  Average speed  :  %-22s  |\n", s_avg);
    printf("   |  Peak (1s)      :  %-22s  |\n", s_peak);
    printf("   |  Downloaded     :  %8.2f GB              |\n", B / 1e9);
    printf("   |  Duration       :  %8.2f s               |\n", T);
    printf("   |  Connections    :  %5d                    |\n", nconn);
    printf("   |  Completed DLs  :  %5lld                    |\n",
           (long long)OK);
    if (ERR > 0)
    printf("   |  Errors         :  %5lld                    |\n",
           (long long)ERR);
    printf("   +============================================+\n\n");

    free(threads);
    free(args);
    return 0;
}
