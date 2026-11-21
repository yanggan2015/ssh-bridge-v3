#ifndef WINNFSD_EMBED_H
#define WINNFSD_EMBED_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* In-process NFSv3 (portmap 111 / nfs 2049 / mount 1058). Windows only. */
int winnfsd_embed_running(void);
int winnfsd_embed_start(const char *export_path, const char *alias_path, unsigned uid, unsigned gid,
                        char *err, size_t errcap);
int winnfsd_embed_stop(void);

#ifdef __cplusplus
}
#endif
#endif
