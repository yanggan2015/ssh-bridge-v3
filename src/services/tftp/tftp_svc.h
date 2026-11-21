#ifndef SSH_BRIDGE_TFTP_SVC_H
#define SSH_BRIDGE_TFTP_SVC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TftpSvc TftpSvc;

TftpSvc *tftp_svc_create(void);
void tftp_svc_destroy(TftpSvc *t);

/* Bind UDP 0.0.0.0:port, serve files under root. allow_write=WRQ. */
int tftp_svc_start(TftpSvc *t, const char *root, int port, int allow_write, char *err, size_t errcap);
/* 同 start，但可指定 bind_ip（空/0.0.0.0 = 自动全部网卡）。 */
int tftp_svc_start_ex(TftpSvc *t, const char *root, const char *bind_ip, int port,
                      int allow_write, char *err, size_t errcap);
void tftp_svc_stop(TftpSvc *t);
int tftp_svc_running(TftpSvc *t);
int tftp_svc_port(TftpSvc *t);
const char *tftp_svc_root(TftpSvc *t);

/* Ensure root/common and root/<name> for each name. */
void tftp_svc_ensure_layout(const char *root, const char **names, int nnames);

/* Jail join: reject .. / abs. Returns 0 and absolute path in out. */
int tftp_safe_join(const char *root, const char *rel, char *out, size_t cap);

/* Fill lowercase hex md5[33]. Returns 0 on ok. */
int tftp_file_md5(const char *path, char md5_hex[33], long long *size_out);

/* Append IPv4 objects {ip,is_192168,is_loopback} to array. Prefer non-loopback. */
int tftp_list_ipv4_json(void *cjson_array /* cJSON* */);

/* Pick default bind_ip into out: first 192.168.*, else first non-loopback, else 127.0.0.1. */
void tftp_pick_default_ip(char *out, size_t cap);

/* Prefer a local IPv4 inside cidr a.b.c.0/24. Returns 0 if filled, -1 if none/invalid. */
int tftp_pick_ip_in_cidr24(const char *cidr, char *out, size_t cap);

/* subnet hint "a.b.c.0/24" from IPv4 dotted quad (best-effort). */
void tftp_subnet_hint(const char *ip, char *out, size_t cap);

/* List directory into cJSON array of {name,is_dir,size}. path must exist. */
int tftp_list_dir_json(const char *abs_dir, void *cjson_array);

#ifdef __cplusplus
}
#endif

#endif
