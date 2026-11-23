#include "nfs_svc.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

static void util_log(const char *fmt, ...) { (void)fmt; }

// NFS 别名会参与导出和 Linux 挂载命令。只允许绝对、单层或多层路径，
// 禁止 ..、引号、空白、反斜杠和控制字符，避免路径越界与 shell 拼接注入。
static bool alias_is_safe(const char *alias) {
    if (!alias || alias[0] != '/') return false;
    size_t n = strlen(alias);
    if (n == 0 || n > 128) return false;
    if (strstr(alias, "..")) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)alias[i];
        if (c < 0x20 || c == 0x7f) return false;
        if (c == '\'' || c == '"' || c == '`' || c == '\\' || c == ';' ||
            c == '&' || c == '|' || c == '$' || c == '(' || c == ')' ||
            c == ' ' || c == '\t' || c == '\n' || c == '\r') return false;
    }
    return true;
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <direct.h>
#include <windows.h>
#include "winnfsd_embed.h"

static void alias_norm(const char *alias, char *out, size_t cap) {
    const char *a = (alias && alias[0]) ? alias : "/share";
    if (a[0] == '/')
        snprintf(out, cap, "%s", a);
    else
        snprintf(out, cap, "/%s", a);
}

// 本工程内部路径统一 UTF-8；winnfsd 第三方层按 ANSI/CP932 处理路径，
// 这里把导出目录转成 8.3 短路径（纯 ASCII）再交给它，避免中文目录直接失败。
static int mkdir_utf8(const char *utf8) {
    wchar_t w[32768];
    if (!utf8 || !utf8[0]) return -1;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 32768) <= 0) return -1;
    if (_wmkdir(w) == 0) return 0;
    return errno == EEXIST ? 0 : -1;
}

static bool to_short_ansi(const char *utf8, char *out, size_t cap) {
    wchar_t w[32768];
    wchar_t sw[32768];
    if (!utf8 || !utf8[0] || !out || cap == 0) return false;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 32768) <= 0) return false;
    DWORD n = GetShortPathNameW(w, sw, 32768);
    if (n == 0 || n >= 32768) return false;
    int m = WideCharToMultiByte(CP_ACP, 0, sw, -1, out, static_cast<int>(cap), nullptr, nullptr);
    if (m <= 0) return false;
    for (int i = 0; i + 1 < m; ++i) {
        if (static_cast<unsigned char>(out[i]) >= 0x80) return false;  // 未拿到纯 ASCII 短路径
    }
    return true;
}

int nfs_svc_running(void) { return winnfsd_embed_running(); }

int nfs_svc_stop(void) {
    return winnfsd_embed_stop() == 0 ? 0 : -1;
}

int nfs_svc_start(const char *export_abs, const char *alias, unsigned uid, unsigned gid,
                  char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    if (!export_abs || !export_abs[0]) {
        if (err) snprintf(err, errcap, "export empty");
        return -1;
    }
    char al[160];
    alias_norm(alias, al, sizeof(al));
    if (!alias_is_safe(al)) {
        if (err) snprintf(err, errcap, "NFS alias must be an absolute path without quotes/..");
        return -1;
    }
    mkdir_utf8(export_abs);
    char export_path[MAX_PATH] = "";
    if (!to_short_ansi(export_abs, export_path, sizeof(export_path))) {
        if (err && errcap)
            snprintf(err, errcap, "导出路径无法转换为 8.3 短路径（中文路径可能不受 winnfsd 支持）");
        return -1;
    }
    if (winnfsd_embed_start(export_path, al, uid, gid, err, errcap) != 0) return -1;
    util_log("[nfs] started export=%s (short=%s) alias=%s", export_abs, export_path, al);
    return 0;
}

void nfs_svc_mount_hint(const char *lan_ip, const char *alias, char *out, size_t cap) {
    char name[128];
    const char *a = (alias && alias[0]) ? alias : "/share";
    const char *ip = (lan_ip && lan_ip[0]) ? lan_ip : "<IP>";
    if (!out || !cap) return;
    if (a[0] == '/') a++;
    snprintf(name, sizeof(name), "%s", a);
    snprintf(out, cap, "mount -t nfs -o nolock,vers=3 %s:/%s /mnt", ip, name);
}

#else

#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define NFS_EXPORTS_D "/etc/exports.d/ssh-bridge.exports"

static volatile int g_nfs_on;
static char g_export_abs[512];
static char g_alias[160];
static int g_we_bind_mounted;

static void alias_norm(const char *alias, char *out, size_t cap) {
    const char *a = (alias && alias[0]) ? alias : "/share";
    if (a[0] == '/')
        snprintf(out, cap, "%s", a);
    else
        snprintf(out, cap, "/%s", a);
}

// 单引号包裹 shell 参数；内部单引号写成 '\''，避免挂载路径含引号时逃逸。
static void shell_quote(const char *s, char *out, size_t cap) {
    size_t j = 0;
    if (!out || cap < 3) return;
    out[j++] = '\'';
    for (const char *p = s ? s : ""; *p && j + 4 < cap; ++p) {
        if (*p == '\'') {
            out[j++] = '\'';
            out[j++] = '\\';
            out[j++] = '\'';
            out[j++] = '\'';
        } else {
            out[j++] = *p;
        }
    }
    out[j++] = '\'';
    out[j] = '\0';
}

static int run_sh(const char *cmd, char *err, size_t errcap) {
    int rc = system(cmd);
    if (rc != 0) {
        if (err && errcap)
            snprintf(err, errcap, "cmd failed(%d): %.180s", rc, cmd);
        return -1;
    }
    return 0;
}

static int ensure_nfs_server(char *err, size_t errcap) {
    if (system("systemctl start nfs-server >/dev/null 2>&1") == 0) return 0;
    if (system("systemctl start nfs-kernel-server >/dev/null 2>&1") == 0) return 0;
    /* already active is fine; try exportfs presence */
    if (system("command -v exportfs >/dev/null 2>&1") == 0) return 0;
    if (err && errcap) snprintf(err, errcap, "nfs-server/exportfs unavailable");
    return -1;
}

static int is_mounted_on(const char *mnt) {
    char cmd[640];
    /* -T matches any covering mount (often "/") — only treat real mountpoint. */
    snprintf(cmd, sizeof(cmd), "mountpoint -q '%s' 2>/dev/null", mnt);
    return system(cmd) == 0;
}

static int write_exports_file(const char *path, unsigned uid, unsigned gid, char *err, size_t errcap) {
    FILE *fp;
    char dir[] = "/etc/exports.d";
    (void)uid;
    (void)gid;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        if (err && errcap) snprintf(err, errcap, "mkdir %s failed", dir);
        return -1;
    }
    fp = fopen(NFS_EXPORTS_D, "w");
    if (!fp) {
        if (err && errcap) snprintf(err, errcap, "write %s failed", NFS_EXPORTS_D);
        return -1;
    }
    /* insecure: boards often use ports >1024; no_root_squash: embedded root */
    fprintf(fp, "%s *(rw,sync,no_subtree_check,no_root_squash,insecure)\n", path);
    fclose(fp);
    return 0;
}

int nfs_svc_running(void) {
    char cmd[700];
    if (!g_nfs_on || !g_alias[0]) return 0;
    snprintf(cmd, sizeof(cmd), "exportfs -v 2>/dev/null | grep -F '%s' >/dev/null 2>&1", g_alias);
    return system(cmd) == 0 ? 1 : 0;
}

int nfs_svc_stop(void) {
    char cmd[700];
    if (g_alias[0]) {
        snprintf(cmd, sizeof(cmd), "exportfs -u *:%s >/dev/null 2>&1 || exportfs -u %s >/dev/null 2>&1",
                 g_alias, g_alias);
        (void)system(cmd);
    }
    unlink(NFS_EXPORTS_D);
    (void)system("exportfs -r >/dev/null 2>&1");
    if (g_we_bind_mounted && g_alias[0]) {
        char q_alias[320];
        shell_quote(g_alias, q_alias, sizeof(q_alias));
        snprintf(cmd, sizeof(cmd), "umount %s >/dev/null 2>&1", q_alias);
        (void)system(cmd);
        g_we_bind_mounted = 0;
    }
    g_nfs_on = 0;
    g_export_abs[0] = 0;
    g_alias[0] = 0;
    util_log("[nfs] stopped");
    return 0;
}

int nfs_svc_start(const char *export_abs, const char *alias, unsigned uid, unsigned gid,
                  char *err, size_t errcap) {
    char al[160], cmd[1200];
    struct stat st;
    if (err && errcap) err[0] = '\0';
    if (!export_abs || !export_abs[0]) {
        if (err) snprintf(err, errcap, "export empty");
        return -1;
    }
    alias_norm(alias, al, sizeof(al));
    if (!alias_is_safe(al)) {
        if (err) snprintf(err, errcap, "NFS alias must be an absolute path without quotes/..");
        return -1;
    }
    if (mkdir(export_abs, 0755) != 0 && errno != EEXIST) {
        if (err) snprintf(err, errcap, "mkdir export failed");
        return -1;
    }
    if (ensure_nfs_server(err, errcap) != 0) return -1;

    /* Board mounts IP:/share — bind real share root onto alias path. */
    if (mkdir(al, 0755) != 0 && errno != EEXIST) {
        if (err) snprintf(err, errcap, "mkdir alias %s failed", al);
        return -1;
    }
    g_we_bind_mounted = 0;
    if (strcmp(export_abs, al) != 0) {
        if (!is_mounted_on(al)) {
            char q_export[640], q_alias[320];
            shell_quote(export_abs, q_export, sizeof(q_export));
            shell_quote(al, q_alias, sizeof(q_alias));
            snprintf(cmd, sizeof(cmd), "mount --bind %s %s", q_export, q_alias);
            if (run_sh(cmd, err, errcap) != 0) return -1;
            g_we_bind_mounted = 1;
        }
    }
    if (write_exports_file(al, uid, gid, err, errcap) != 0) return -1;
    if (run_sh("exportfs -ra", err, errcap) != 0) return -1;

    snprintf(g_export_abs, sizeof(g_export_abs), "%s", export_abs);
    snprintf(g_alias, sizeof(g_alias), "%s", al);
    g_nfs_on = 1;
    if (stat(al, &st) != 0) {
        /* still mark on; exportfs may succeed */
    }
    util_log("[nfs] started export=%s alias=%s", export_abs, al);
    if (err && errcap) err[0] = 0;
    return 0;
}

void nfs_svc_mount_hint(const char *lan_ip, const char *alias, char *out, size_t cap) {
    char name[128];
    const char *a = (alias && alias[0]) ? alias : "/share";
    const char *ip = (lan_ip && lan_ip[0]) ? lan_ip : "<IP>";
    if (!out || !cap) return;
    if (a[0] == '/') a++;
    snprintf(name, sizeof(name), "%s", a);
    snprintf(out, cap, "mount -t nfs -o nolock,vers=3,tcp %s:/%s /mnt", ip, name);
}

#endif
