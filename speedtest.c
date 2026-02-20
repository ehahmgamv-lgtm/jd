/*
 * speedtest.c — оценка пропускной способности сети (download)
 *
 * Компиляция:
 *   gcc -O2 -o speedtest speedtest.c
 *
 * Использование:
 *   ./speedtest                  # 3 прогона по ~100 MB
 *   ./speedtest 25000000 5       # 5 прогонов по ~25 MB
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netdb.h>

#define HOST            "speed.cloudflare.com"
#define PORT            "80"
#define DEFAULT_BYTES   99999999L
#define DEFAULT_RUNS    3
#define READ_BUF        (128 * 1024)
#define RCVBUF_SIZE     (1 * 1024 * 1024)

/* ─── Монотонные часы ─── */

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ─── Прогресс-бар ─── */

static void print_progress(long long received, long long expected, double elapsed)
{
    double pct   = expected > 0 ? (double)received / expected * 100.0 : 0;
    double mb    = received / 1e6;
    double speed = elapsed > 0.001 ? (received * 8.0 / 1e6) / elapsed : 0;

    fprintf(stderr, "\r  [%5.1f%%]  %.2f / %.2f MB   %8.2f Mbps",
            pct, mb, expected / 1e6, speed);
    fflush(stderr);
}

/* ─── TCP-соединение ─── */

static int tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo(%s): %s\n", host, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        fprintf(stderr, "Не удалось подключиться к %s:%s\n", host, port);
        return -1;
    }

    int rcvbuf = RCVBUF_SIZE;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    return fd;
}

/* ─── Поиск подстроки без учёта регистра ─── */

static char *my_strcasestr(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0) return (char *)haystack;
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
    }
    return NULL;
}

/* ─── Один прогон загрузки ─── */

typedef struct {
    long long body_bytes;
    double    elapsed;
    int       http_status;
} download_result_t;

static int run_download(long req_bytes, download_result_t *out)
{
    memset(out, 0, sizeof(*out));

    /* 1. TCP-соединение */
    int fd = tcp_connect(HOST, PORT);
    if (fd < 0) return -1;

    /* 2. HTTP-запрос */
    char req[512];
    int rlen = snprintf(req, sizeof(req),
            "GET /__down?bytes=%ld HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: speedtest-c/1.0\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n"
            "\r\n",
            req_bytes, HOST);

    ssize_t sent = 0;
    while (sent < rlen) {
        ssize_t n = write(fd, req + sent, rlen - sent);
        if (n <= 0) {
            fprintf(stderr, "write() failed: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
        sent += n;
    }

    /* 3. Чтение ответа */
    char *buf = malloc(READ_BUF);
    if (!buf) { close(fd); return -1; }

    char  *hdr_buf      = NULL;
    int    hdr_len       = 0;
    int    headers_done  = 0;
    long long content_length = req_bytes;

    double    t_start    = 0;
    long long total_body = 0;

    for (;;) {
        ssize_t n = read(fd, buf, READ_BUF);
        if (n <= 0) break;

        if (!headers_done) {
            hdr_buf = realloc(hdr_buf, hdr_len + n + 1);
            if (!hdr_buf) { free(buf); close(fd); return -1; }
            memcpy(hdr_buf + hdr_len, buf, n);
            hdr_len += n;
            hdr_buf[hdr_len] = '\0';

            char *sep = strstr(hdr_buf, "\r\n\r\n");
            if (!sep) continue;

            sep += 4;
            int body_in_hdr = hdr_len - (int)(sep - hdr_buf);

            /* HTTP-статус */
            if (sscanf(hdr_buf, "HTTP/%*s %d", &out->http_status) != 1)
                out->http_status = 0;

            /* Content-Length */
            const char *cl = my_strcasestr(hdr_buf, "Content-Length:");
            if (cl) content_length = atoll(cl + 15);

            headers_done = 1;
            t_start      = now_sec();
            total_body   = body_in_hdr;

            free(hdr_buf);
            hdr_buf = NULL;

            print_progress(total_body, content_length, 0);
        } else {
            total_body += n;
            print_progress(total_body, content_length, now_sec() - t_start);
        }
    }

    double t_end = now_sec();
    free(buf);

    fprintf(stderr, "\r%70s\r", "");

    if (!headers_done) {
        fprintf(stderr, "Не удалось получить заголовки HTTP\n");
        if (hdr_buf) free(hdr_buf);
        close(fd);
        return -1;
    }

    out->body_bytes = total_body;
    out->elapsed    = t_end - t_start;
    if (out->elapsed < 1e-6) out->elapsed = 1e-6;

    close(fd);
    return 0;
}

/* ─── main ─── */

int main(int argc, char *argv[])
{
    long download_bytes = DEFAULT_BYTES;
    int  num_runs       = DEFAULT_RUNS;

    if (argc > 1) download_bytes = atol(argv[1]);
    if (argc > 2) num_runs       = atoi(argv[2]);
    if (download_bytes <= 0) download_bytes = DEFAULT_BYTES;
    if (num_runs <= 0)       num_runs       = DEFAULT_RUNS;

    printf("======================================\n");
    printf("    Network Speed Test (download)\n");
    printf("======================================\n\n");
    printf("  Сервер:        %s:%s\n", HOST, PORT);
    printf("  Размер:        %.2f MB\n", download_bytes / 1e6);
    printf("  Кол-во тестов: %d\n\n", num_runs);

    double speeds[num_runs];
    int    ok = 0;

    for (int i = 0; i < num_runs; i++) {
        printf("  Тест %d/%d …\n", i + 1, num_runs);

        download_result_t res;
        if (run_download(download_bytes, &res) < 0) {
            printf("    ОШИБКА: соединение не удалось\n\n");
            continue;
        }

        if (res.http_status != 200) {
            printf("    ОШИБКА: HTTP %d\n\n", res.http_status);
            continue;
        }

        double mbps = (res.body_bytes * 8.0 / 1e6) / res.elapsed;
        speeds[ok++] = mbps;

        printf("    Получено: %.2f MB за %.2f с  =>  %.2f Mbps\n\n",
               res.body_bytes / 1e6, res.elapsed, mbps);
    }

    if (ok == 0) {
        printf("  Все тесты завершились с ошибкой.\n");
        return EXIT_FAILURE;
    }

    double sum = 0, best = 0, worst = 1e18;
    for (int i = 0; i < ok; i++) {
        sum += speeds[i];
        if (speeds[i] > best)  best  = speeds[i];
        if (speeds[i] < worst) worst = speeds[i];
    }

    printf("  +------------------------------------+\n");
    printf("  |  Средняя скорость: %8.2f Mbps   |\n", sum / ok);
    printf("  |  Лучшая:          %8.2f Mbps   |\n", best);
    printf("  |  Худшая:          %8.2f Mbps   |\n", worst);
    printf("  |  Успешных тестов:  %d / %d            |\n", ok, num_runs);
    printf("  +------------------------------------+\n");

    return EXIT_SUCCESS;
}
