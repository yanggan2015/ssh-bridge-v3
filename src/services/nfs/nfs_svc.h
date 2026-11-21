#ifndef SSH_BRIDGE_NFS_SVC_H
#define SSH_BRIDGE_NFS_SVC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NFSv3: Windows=WinNFSd embed; Linux=kernel nfs-server + /etc/exports.d. */
int nfs_svc_running(void);
int nfs_svc_start(const char *export_abs, const char *alias, unsigned uid, unsigned gid,
                  char *err, size_t errcap);
int nfs_svc_stop(void);
void nfs_svc_mount_hint(const char *lan_ip, const char *alias, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
