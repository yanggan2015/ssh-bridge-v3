#include "tftp_svc.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <wchar.h>

static FILE *fopen_utf8(const char *path, const char *mode) {
    wchar_t wpath[1024], wmode[16];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) <= 0) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 16) <= 0) return NULL;
    return _wfopen(wpath, wmode);
}

static int mkdir_utf8(const char *path) {
    wchar_t wpath[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) <= 0) return -1;
    return _wmkdir(wpath);
}
#else
static FILE *fopen_utf8(const char *path, const char *mode) { return fopen(path, mode); }
static int mkdir_utf8(const char *path) { return mkdir(path, 0777); }
#endif

#define CFG_PATH 512

static void util_log(const char *fmt, ...) {
    (void)fmt;   /* 服务日志由上层 UI/状态栏呈现，这里不额外落盘 */
}

static void ensure_dir(const char *path) {
    char parent[CFG_PATH];
    size_t n;
    if (!path || !path[0]) return;
#ifdef _WIN32
    if (mkdir_utf8(path) == 0 || errno == EEXIST) return;
#else
    if (mkdir(path, 0777) == 0 || errno == EEXIST) return;
#endif
    n = strlen(path);
    while (n > 0 && path[n - 1] != '/' && path[n - 1] != '\\') --n;
    if (n <= 1) return;
    snprintf(parent, sizeof(parent), "%s", path);
    parent[n - 1] = 0;
    ensure_dir(parent);
#ifdef _WIN32
    mkdir_utf8(path);
#else
    mkdir(path, 0777);
#endif
}

static void util_path_join(char *dst, size_t cap, const char *a, const char *b) {
    size_t n;
    if (!dst || !cap) return;
    snprintf(dst, cap, "%s", a ? a : "");
    n = strlen(dst);
    if (n && dst[n - 1] != '/' && dst[n - 1] != '\\' && n + 1 < cap) {
        dst[n++] = '\\';
        dst[n] = 0;
    }
    snprintf(dst + n, cap - n, "%s", b ? b : "");
}

static void util_dirname(char *dst, size_t cap, const char *path) {
    size_t n;
    if (!dst || !cap) return;
    snprintf(dst, cap, "%s", path ? path : "");
    n = strlen(dst);
    while (n > 0 && dst[n - 1] != '/' && dst[n - 1] != '\\') --n;
    if (n > 0) {
        while (n > 1 && (dst[n - 1] == '/' || dst[n - 1] == '\\')) --n;
        dst[n] = 0;
    } else {
        dst[0] = 0;
    }
}

#ifdef _WIN32
typedef SOCKET tftp_sock_t;
#define TFTP_INVALID INVALID_SOCKET
#define tftp_close closesocket
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
typedef int tftp_sock_t;
#define TFTP_INVALID (-1)
#define tftp_close close
#endif

#include <pthread.h>

#define TFTP_RRQ 1
#define TFTP_WRQ 2
#define TFTP_DATA 3
#define TFTP_ACK 4
#define TFTP_ERROR 5
#define TFTP_BLK 512
#define TFTP_BUFSZ (4 + TFTP_BLK)

struct TftpSvc {
    volatile int running;
    volatile int allow_write;
    int port;
    char root[CFG_PATH];
    tftp_sock_t sock;
    pthread_t th;
    int th_on;
};

/* --- MD5 (public-domain compact) --- */
typedef struct {
    uint32_t s[4];
    uint64_t bits;
    unsigned char buf[64];
    size_t nbuf;
} Md5Ctx;

static uint32_t md5_rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void md5_init(Md5Ctx *c) {
    c->s[0] = 0x67452301;
    c->s[1] = 0xefcdab89;
    c->s[2] = 0x98badcfe;
    c->s[3] = 0x10325476;
    c->bits = 0;
    c->nbuf = 0;
}

static void md5_block(Md5Ctx *c, const unsigned char block[64]) {
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const int S[64] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                              4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    uint32_t a = c->s[0], b = c->s[1], cc = c->s[2], d = c->s[3], M[16];
    int i;
    for (i = 0; i < 16; ++i)
        M[i] = (uint32_t)block[i * 4] | ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) | ((uint32_t)block[i * 4 + 3] << 24);
    for (i = 0; i < 64; ++i) {
        uint32_t f, g;
        if (i < 16) {
            f = (b & cc) | ((~b) & d);
            g = (uint32_t)i;
        } else if (i < 32) {
            f = (d & b) | ((~d) & cc);
            g = (uint32_t)(5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ cc ^ d;
            g = (uint32_t)(3 * i + 5) % 16;
        } else {
            f = cc ^ (b | (~d));
            g = (uint32_t)(7 * i) % 16;
        }
        f = f + a + K[i] + M[g];
        a = d;
        d = cc;
        cc = b;
        b = b + md5_rol(f, S[i]);
    }
    c->s[0] += a;
    c->s[1] += b;
    c->s[2] += cc;
    c->s[3] += d;
}

static void md5_update(Md5Ctx *c, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    c->bits += (uint64_t)n * 8;
    while (n) {
        size_t take = 64 - c->nbuf;
        if (take > n) take = n;
        memcpy(c->buf + c->nbuf, p, take);
        c->nbuf += take;
        p += take;
        n -= take;
        if (c->nbuf == 64) {
            md5_block(c, c->buf);
            c->nbuf = 0;
        }
    }
}

static void md5_final(Md5Ctx *c, unsigned char out[16]) {
    size_t i;
    unsigned char pad[64];
    uint64_t bits = c->bits;
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    if (c->nbuf < 56)
        md5_update(c, pad, 56 - c->nbuf);
    else {
        md5_update(c, pad, 64 - c->nbuf);
        md5_update(c, pad + 1, 56);
    }
    for (i = 0; i < 8; ++i) pad[i] = (unsigned char)((bits >> (8 * i)) & 0xff);
    md5_update(c, pad, 8);
    for (i = 0; i < 4; ++i) {
        out[i * 4] = (unsigned char)(c->s[i] & 0xff);
        out[i * 4 + 1] = (unsigned char)((c->s[i] >> 8) & 0xff);
        out[i * 4 + 2] = (unsigned char)((c->s[i] >> 16) & 0xff);
        out[i * 4 + 3] = (unsigned char)((c->s[i] >> 24) & 0xff);
    }
}

int tftp_file_md5(const char *path, char md5_hex[33], long long *size_out) {
    FILE *f;
    Md5Ctx ctx;
    unsigned char buf[8192], dig[16];
    long long sz = 0;
    size_t n;
    int i;
    if (!path || !md5_hex) return -1;
    f = fopen_utf8(path, "rb");
    if (!f) return -1;
    md5_init(&ctx);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        md5_update(&ctx, buf, n);
        sz += (long long)n;
    }
    fclose(f);
    md5_final(&ctx, dig);
    for (i = 0; i < 16; ++i) sprintf(md5_hex + i * 2, "%02x", dig[i]);
    md5_hex[32] = 0;
    if (size_out) *size_out = sz;
    return 0;
}

int tftp_safe_join(const char *root, const char *rel, char *out, size_t cap) {
    char norm[CFG_PATH];
    size_t i, j = 0;
    if (!root || !root[0] || !out || cap < 8) return -1;
    if (!rel) rel = "";
    while (*rel == '/' || *rel == '\\') rel++;
    for (i = 0; rel[i] && j + 1 < sizeof(norm); ++i) {
        char ch = rel[i];
        if (ch == '\\') ch = '/';
        if (ch == '\0') break;
        norm[j++] = ch;
    }
    norm[j] = 0;
    if (strstr(norm, "..") || norm[0] == '/' || (j >= 2 && norm[1] == ':')) return -1;
    if (!norm[0]) {
        snprintf(out, cap, "%s", root);
        return 0;
    }
    util_path_join(out, cap, root, norm);
    /* Ensure result stays under root */
    {
        size_t rl = strlen(root);
        if (strncmp(out, root, rl) != 0) return -1;
        if (out[rl] != 0 && out[rl] != '/' && out[rl] != '\\') return -1;
    }
    return 0;
}

void tftp_svc_ensure_layout(const char *root, const char **names, int nnames) {
    char p[CFG_PATH];
    int i;
    if (!root || !root[0]) return;
    ensure_dir(root);
    util_path_join(p, sizeof(p), root, "common");
    ensure_dir(p);
    for (i = 0; i < nnames; ++i) {
        if (!names[i] || !names[i][0]) continue;
        if (strchr(names[i], '/') || strchr(names[i], '\\') || strstr(names[i], "..")) continue;
        util_path_join(p, sizeof(p), root, names[i]);
        ensure_dir(p);
    }
}

void tftp_subnet_hint(const char *ip, char *out, size_t cap) {
    unsigned a, b, c, d;
    if (!out || !cap) return;
    out[0] = 0;
    if (!ip || sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return;
    snprintf(out, cap, "%u.%u.%u.0/24", a, b, c);
}

void tftp_pick_default_ip(char *out, size_t cap) {
    cJSON *arr = cJSON_CreateArray();
    cJSON *it;
    const char *fallback = "127.0.0.1";
    if (!out || !cap) return;
    out[0] = 0;
    if (!arr) {
        snprintf(out, cap, "%s", fallback);
        return;
    }
    tftp_list_ipv4_json(arr);
    for (it = arr->child; it; it = it->next) {
        cJSON *ip = cJSON_GetObjectItemCaseSensitive(it, "ip");
        cJSON *is192 = cJSON_GetObjectItemCaseSensitive(it, "is_192168");
        if (cJSON_IsString(ip) && cJSON_IsTrue(is192)) {
            snprintf(out, cap, "%s", ip->valuestring);
            cJSON_Delete(arr);
            return;
        }
    }
    for (it = arr->child; it; it = it->next) {
        cJSON *ip = cJSON_GetObjectItemCaseSensitive(it, "ip");
        cJSON *loop = cJSON_GetObjectItemCaseSensitive(it, "is_loopback");
        if (cJSON_IsString(ip) && !cJSON_IsTrue(loop)) {
            snprintf(out, cap, "%s", ip->valuestring);
            cJSON_Delete(arr);
            return;
        }
    }
    snprintf(out, cap, "%s", fallback);
    cJSON_Delete(arr);
}

int tftp_pick_ip_in_cidr24(const char *cidr, char *out, size_t cap) {
    unsigned a, b, c, d, bits;
    char slash, tail;
    cJSON *arr, *it;
    if (!out || !cap) return -1;
    out[0] = 0;
    if (!cidr || !cidr[0]) return -1;
    if (sscanf(cidr, "%u.%u.%u.%u%c%u%c", &a, &b, &c, &d, &slash, &bits, &tail) != 6 ||
        slash != '/' || bits != 24 || a > 255 || b > 255 || c > 255 || d != 0)
        return -1;
    arr = cJSON_CreateArray();
    if (!arr) return -1;
    tftp_list_ipv4_json(arr);
    for (it = arr->child; it; it = it->next) {
        cJSON *ip = cJSON_GetObjectItemCaseSensitive(it, "ip");
        unsigned xa, xb, xc, xd;
        if (!cJSON_IsString(ip) || !ip->valuestring) continue;
        if (sscanf(ip->valuestring, "%u.%u.%u.%u", &xa, &xb, &xc, &xd) != 4) continue;
        if (xa == a && xb == b && xc == c) {
            snprintf(out, cap, "%s", ip->valuestring);
            cJSON_Delete(arr);
            return 0;
        }
    }
    cJSON_Delete(arr);
    return -1;
}

int tftp_list_ipv4_json(void *cjson_array) {
    cJSON *arr = (cJSON *)cjson_array;
    if (!arr) return -1;
#ifdef _WIN32
    {
        ULONG sz = 0;
        IP_ADAPTER_ADDRESSES *addrs = NULL, *a;
        DWORD rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                                       GAA_FLAG_SKIP_DNS_SERVER,
                                        NULL, NULL, &sz);
        if (rc != ERROR_BUFFER_OVERFLOW) return -1;
        addrs = (IP_ADAPTER_ADDRESSES *)malloc(sz);
        if (!addrs) return -1;
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                                 GAA_FLAG_SKIP_DNS_SERVER,
                                  NULL, addrs, &sz);
        if (rc != NO_ERROR) {
            free(addrs);
            return -1;
        }
        for (a = addrs; a; a = a->Next) {
            IP_ADAPTER_UNICAST_ADDRESS *u;
            if (a->OperStatus != IfOperStatusUp) continue;
            for (u = a->FirstUnicastAddress; u; u = u->Next) {
                SOCKADDR_IN *sa = (SOCKADDR_IN *)u->Address.lpSockaddr;
                char ip[64];
                cJSON *o;
                int loop, is192;
                if (!sa || sa->sin_family != AF_INET) continue;
                inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                loop = (strncmp(ip, "127.", 4) == 0);
                is192 = (strncmp(ip, "192.168.", 8) == 0);
                o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "ip", ip);
                cJSON_AddBoolToObject(o, "is_192168", is192);
                cJSON_AddBoolToObject(o, "is_loopback", loop);
                cJSON_AddItemToArray(arr, o);
            }
        }
        free(addrs);
        return 0;
    }
#else
    {
        struct ifaddrs *ifa = NULL, *p;
        if (getifaddrs(&ifa) != 0) return -1;
        for (p = ifa; p; p = p->ifa_next) {
            struct sockaddr_in *sa;
            char ip[64];
            cJSON *o;
            int loop, is192;
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            sa = (struct sockaddr_in *)p->ifa_addr;
            inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
            loop = (strncmp(ip, "127.", 4) == 0);
            is192 = (strncmp(ip, "192.168.", 8) == 0);
            o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "ip", ip);
            cJSON_AddBoolToObject(o, "is_192168", is192);
            cJSON_AddBoolToObject(o, "is_loopback", loop);
            cJSON_AddItemToArray(arr, o);
        }
        freeifaddrs(ifa);
        return 0;
    }
#endif
}

static void tftp_send_error(tftp_sock_t s, const struct sockaddr_in *peer, int code, const char *msg) {
    unsigned char buf[4 + 128];
    size_t n = 0;
    buf[0] = 0;
    buf[1] = TFTP_ERROR;
    buf[2] = (unsigned char)((code >> 8) & 0xff);
    buf[3] = (unsigned char)(code & 0xff);
    n = 4;
    if (msg) {
        size_t m = strlen(msg);
        if (m > 120) m = 120;
        memcpy(buf + 4, msg, m);
        n += m;
    }
    buf[n++] = 0;
    sendto(s, (const char *)buf, (int)n, 0, (const struct sockaddr *)peer, sizeof(*peer));
}

static void tftp_send_ack(tftp_sock_t s, const struct sockaddr_in *peer, unsigned block) {
    unsigned char buf[4];
    buf[0] = 0;
    buf[1] = TFTP_ACK;
    buf[2] = (unsigned char)((block >> 8) & 0xff);
    buf[3] = (unsigned char)(block & 0xff);
    sendto(s, (const char *)buf, 4, 0, (const struct sockaddr *)peer, sizeof(*peer));
}

static int tftp_recv_timeout(tftp_sock_t s, unsigned char *buf, int cap, struct sockaddr_in *from,
                             int timeout_ms) {
    fd_set rfds;
    struct timeval tv;
    int n;
    socklen_t flen = sizeof(*from);
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    n = select((int)s + 1, &rfds, NULL, NULL, &tv);
    if (n <= 0) return -1;
    return recvfrom(s, (char *)buf, cap, 0, (struct sockaddr *)from, &flen);
}

static void tftp_do_rrq(TftpSvc *t, tftp_sock_t ls, struct sockaddr_in peer, const char *filename) {
    char path[CFG_PATH];
    FILE *f;
    unsigned char buf[TFTP_BUFSZ], rbuf[TFTP_BUFSZ];
    unsigned block = 1;
    size_t nread;
    tftp_sock_t ds;
    struct sockaddr_in local;
    if (tftp_safe_join(t->root, filename, path, sizeof(path)) != 0) {
        tftp_send_error(ls, &peer, 1, "bad path");
        return;
    }
    f = fopen_utf8(path, "rb");
    if (!f) {
        tftp_send_error(ls, &peer, 1, "file not found");
        return;
    }
    ds = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ds == TFTP_INVALID) {
        fclose(f);
        return;
    }
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    bind(ds, (struct sockaddr *)&local, sizeof(local));
    for (;;) {
        int tries = 0, ack_ok = 0;
        nread = fread(buf + 4, 1, TFTP_BLK, f);
        buf[0] = 0;
        buf[1] = TFTP_DATA;
        buf[2] = (unsigned char)((block >> 8) & 0xff);
        buf[3] = (unsigned char)(block & 0xff);
        while (tries < 5 && !ack_ok) {
            struct sockaddr_in from;
            int rn;
            sendto(ds, (const char *)buf, (int)(4 + nread), 0, (struct sockaddr *)&peer, sizeof(peer));
            rn = tftp_recv_timeout(ds, rbuf, (int)sizeof(rbuf), &from, 2000);
            if (rn >= 4 && rbuf[1] == TFTP_ACK) {
                unsigned ab = ((unsigned)rbuf[2] << 8) | rbuf[3];
                if (ab == block) ack_ok = 1;
            }
            tries++;
        }
        if (!ack_ok) break;
        if (nread < TFTP_BLK) break;
        block++;
        if (block == 0) block = 1;
    }
    fclose(f);
    tftp_close(ds);
}

static void tftp_do_wrq(TftpSvc *t, tftp_sock_t ls, struct sockaddr_in peer, const char *filename) {
    char path[CFG_PATH], dir[CFG_PATH];
    FILE *f;
    unsigned char buf[TFTP_BUFSZ];
    unsigned expect = 1;
    tftp_sock_t ds;
    struct sockaddr_in local;
    if (!t->allow_write) {
        tftp_send_error(ls, &peer, 2, "write disabled");
        return;
    }
    if (tftp_safe_join(t->root, filename, path, sizeof(path)) != 0) {
        tftp_send_error(ls, &peer, 1, "bad path");
        return;
    }
    util_dirname(dir, sizeof(dir), path);
    ensure_dir(dir);
    f = fopen_utf8(path, "wb");
    if (!f) {
        tftp_send_error(ls, &peer, 2, "cannot create");
        return;
    }
    ds = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ds == TFTP_INVALID) {
        fclose(f);
        return;
    }
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    bind(ds, (struct sockaddr *)&local, sizeof(local));
    tftp_send_ack(ds, &peer, 0);
    for (;;) {
        struct sockaddr_in from;
        int rn = tftp_recv_timeout(ds, buf, (int)sizeof(buf), &from, 5000);
        unsigned blk;
        size_t data_n;
        if (rn < 4 || buf[1] != TFTP_DATA) break;
        blk = ((unsigned)buf[2] << 8) | buf[3];
        if (blk != expect) {
            tftp_send_ack(ds, &peer, expect - 1);
            continue;
        }
        data_n = (size_t)rn - 4;
        if (data_n && fwrite(buf + 4, 1, data_n, f) != data_n) break;
        tftp_send_ack(ds, &peer, blk);
        if (data_n < TFTP_BLK) break;
        expect++;
        if (expect == 0) expect = 1;
    }
    fclose(f);
    tftp_close(ds);
}

static void *tftp_thread(void *arg) {
    TftpSvc *t = (TftpSvc *)arg;
    while (t->running) {
        unsigned char buf[TFTP_BUFSZ];
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        fd_set rfds;
        struct timeval tv;
        int n;
        FD_ZERO(&rfds);
        FD_SET(t->sock, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        n = select((int)t->sock + 1, &rfds, NULL, NULL, &tv);
        if (n <= 0) continue;
        n = recvfrom(t->sock, (char *)buf, (int)sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n < 4) continue;
        if (buf[1] == TFTP_RRQ || buf[1] == TFTP_WRQ) {
            char filename[256], mode[32];
            int i = 2, fi = 0, mi = 0;
            while (i < n && buf[i] && fi + 1 < (int)sizeof(filename)) filename[fi++] = (char)buf[i++];
            filename[fi] = 0;
            if (i < n && buf[i] == 0) i++;
            while (i < n && buf[i] && mi + 1 < (int)sizeof(mode)) mode[mi++] = (char)buf[i++];
            mode[mi] = 0;
            (void)mode;
            if (buf[1] == TFTP_RRQ)
                tftp_do_rrq(t, t->sock, from, filename);
            else
                tftp_do_wrq(t, t->sock, from, filename);
        }
    }
    return NULL;
}

TftpSvc *tftp_svc_create(void) {
    TftpSvc *t = (TftpSvc *)calloc(1, sizeof(TftpSvc));
    if (t) t->sock = TFTP_INVALID;
    return t;
}

void tftp_svc_stop(TftpSvc *t) {
    if (!t) return;
    t->running = 0;
    if (t->th_on) {
        pthread_join(t->th, NULL);
        t->th_on = 0;
    }
    if (t->sock != TFTP_INVALID) {
        tftp_close(t->sock);
        t->sock = TFTP_INVALID;
    }
}

void tftp_svc_destroy(TftpSvc *t) {
    if (!t) return;
    tftp_svc_stop(t);
    free(t);
}

int tftp_svc_start_ex(TftpSvc *t, const char *root, const char *bind_ip, int port,
                      int allow_write, char *err, size_t errcap) {
    struct sockaddr_in addr;
#ifndef _WIN32
    int yes = 1;
#endif
    if (!t || !root || !root[0]) {
        if (err && errcap) snprintf(err, errcap, "bad args");
        return -1;
    }
    tftp_svc_stop(t);
    ensure_dir(root);
    snprintf(t->root, sizeof(t->root), "%s", root);
    t->port = port > 0 ? port : 69;
    if (t->port > 65535) {
        if (err && errcap) snprintf(err, errcap, "端口超出 1-65535");
        t->port = 69;
        return -1;
    }
    t->allow_write = allow_write ? 1 : 0;
    t->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (t->sock == TFTP_INVALID) {
        if (err && errcap) snprintf(err, errcap, "socket failed");
        return -1;
    }
#ifdef _WIN32
    {
        BOOL exclusive = TRUE;
        setsockopt(t->sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive));
    }
#else
    setsockopt(t->sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
#endif
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind_ip && bind_ip[0] && strcmp(bind_ip, "0.0.0.0") != 0) {
        addr.sin_addr.s_addr = inet_addr(bind_ip);
    }
    addr.sin_port = htons((unsigned short)t->port);
    if (bind(t->sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
#ifndef _WIN32
        /* Linux: tftpd-hpa often owns :69 — stop it once and retry. */
        if (errno == EADDRINUSE && t->port == 69) {
            (void)system("systemctl stop tftpd-hpa >/dev/null 2>&1");
            (void)system("systemctl stop tftpd >/dev/null 2>&1");
            (void)system("systemctl disable tftpd-hpa >/dev/null 2>&1");
            if (bind(t->sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) goto tftp_bound;
        }
#endif
        if (err && errcap)
            snprintf(err, errcap, "UDP 端口 %d 绑定失败（该端口可能已被其他 TFTP 服务占用）", t->port);
        tftp_close(t->sock);
        t->sock = TFTP_INVALID;
        return -1;
    }
#ifndef _WIN32
tftp_bound:
#endif
    t->running = 1;
    if (pthread_create(&t->th, NULL, tftp_thread, t) != 0) {
        t->running = 0;
        tftp_close(t->sock);
        t->sock = TFTP_INVALID;
        if (err && errcap) snprintf(err, errcap, "thread failed");
        return -1;
    }
    t->th_on = 1;
    util_log("[tftp] listening UDP %d root=%s", t->port, t->root);
    if (err && errcap) err[0] = 0;
    return 0;
}

int tftp_svc_start(TftpSvc *t, const char *root, int port, int allow_write, char *err, size_t errcap) {
    return tftp_svc_start_ex(t, root, NULL, port, allow_write, err, errcap);
}

int tftp_svc_running(TftpSvc *t) { return t && t->running && t->th_on; }
int tftp_svc_port(TftpSvc *t) { return t ? t->port : 0; }
const char *tftp_svc_root(TftpSvc *t) { return t ? t->root : ""; }

int tftp_list_dir_json(const char *abs_dir, void *cjson_array) {
    cJSON *arr = (cJSON *)cjson_array;
    if (!abs_dir || !arr) return -1;
#ifdef _WIN32
    {
        char pattern[CFG_PATH];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pattern, sizeof(pattern), "%s\\*", abs_dir);
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return -1;
        do {
            cJSON *o;
            if (fd.cFileName[0] == '.' &&
                (fd.cFileName[1] == 0 || (fd.cFileName[1] == '.' && fd.cFileName[2] == 0)))
                continue;
            o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", fd.cFileName);
            cJSON_AddBoolToObject(o, "is_dir", (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
            cJSON_AddNumberToObject(
                o, "size",
                (double)(((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow));
            cJSON_AddItemToArray(arr, o);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        return 0;
    }
#else
    {
        DIR *d = opendir(abs_dir);
        struct dirent *e;
        if (!d) return -1;
        while ((e = readdir(d)) != NULL) {
            char full[CFG_PATH];
            struct stat st;
            cJSON *o;
            if (e->d_name[0] == '.' &&
                (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0)))
                continue;
            util_path_join(full, sizeof(full), abs_dir, e->d_name);
            if (stat(full, &st) != 0) continue;
            o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", e->d_name);
            cJSON_AddBoolToObject(o, "is_dir", S_ISDIR(st.st_mode));
            cJSON_AddNumberToObject(o, "size", (double)st.st_size);
            cJSON_AddItemToArray(arr, o);
        }
        closedir(d);
        return 0;
    }
#endif
}

