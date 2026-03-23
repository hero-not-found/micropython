#include "py/mpconfig.h"

#if MICROPY_VFS && MICROPY_VFS_FAT

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "py/mperrno.h"
#include "py/mpthread.h"
#include "extmod/vfs_fat_native.h"

#if MICROPY_PY_THREAD && FF_FS_REENTRANT

struct _mp_native_fat_mount_entry_t {
    fs_user_mount_t *mount;
    char *mount_path;
    size_t mount_len;
    size_t active_handles;
    bool mounted;
    struct _mp_native_fat_mount_entry_t *next;
};

static mp_thread_mutex_t native_fat_registry_mutex;
static bool native_fat_registry_inited;
static mp_native_fat_mount_entry_t *native_fat_registry_head;

static int native_fat_registry_ensure_inited(void) {
    if (!native_fat_registry_inited) {
        mp_thread_mutex_init(&native_fat_registry_mutex);
        native_fat_registry_inited = true;
    }
    return 0;
}

static void native_fat_registry_lock(void) {
    mp_thread_mutex_lock(&native_fat_registry_mutex, 1);
}

static void native_fat_registry_unlock(void) {
    mp_thread_mutex_unlock(&native_fat_registry_mutex);
}

static bool native_fat_mount_matches_path(const mp_native_fat_mount_entry_t *entry, const char *path) {
    if (entry->mount_len == 1) {
        return path[0] == '/';
    }
    if (strncmp(path, entry->mount_path, entry->mount_len) != 0) {
        return false;
    }
    return path[entry->mount_len] == '\0' || path[entry->mount_len] == '/';
}

static const char *native_fat_path_within_mount(const mp_native_fat_mount_entry_t *entry, const char *path) {
    if (entry->mount_len == 1) {
        return path;
    }
    if (path[entry->mount_len] == '\0') {
        return "/";
    }
    return path + entry->mount_len;
}

static mp_native_fat_mount_entry_t *native_fat_find_entry_for_mount(mp_vfs_mount_t *vfs) {
    if (!mp_obj_is_type(vfs->obj, &mp_fat_vfs_type)) {
        return NULL;
    }
    fs_user_mount_t *mount = MP_OBJ_TO_PTR(vfs->obj);
    for (mp_native_fat_mount_entry_t *entry = native_fat_registry_head; entry != NULL; entry = entry->next) {
        if (entry->mount == mount) {
            return entry;
        }
    }
    return NULL;
}

static mp_native_fat_mount_entry_t *native_fat_find_entry_for_path(const char *path, const char **path_out) {
    mp_native_fat_mount_entry_t *best = NULL;
    for (mp_native_fat_mount_entry_t *entry = native_fat_registry_head; entry != NULL; entry = entry->next) {
        if (!entry->mounted || !native_fat_mount_matches_path(entry, path)) {
            continue;
        }
        if (best == NULL || entry->mount_len > best->mount_len) {
            best = entry;
        }
    }
    if (best != NULL && path_out != NULL) {
        *path_out = native_fat_path_within_mount(best, path);
    }
    return best;
}

static int native_fat_flags_to_mode(int flags, BYTE *mode_out, bool *append_out) {
    BYTE mode = 0;
    bool append = (flags & MP_NATIVE_FAT_O_APPEND) != 0;
    bool truncate = (flags & MP_NATIVE_FAT_O_TRUNCATE) != 0;
    bool create = (flags & MP_NATIVE_FAT_O_CREATE) != 0;
    bool want_read = (flags & MP_NATIVE_FAT_O_READ) != 0;
    bool want_write = (flags & MP_NATIVE_FAT_O_WRITE) != 0;

    if (!want_read && !want_write) {
        return -MP_EINVAL;
    }
    if ((append || truncate || create) && !want_write) {
        return -MP_EINVAL;
    }
    if (append && truncate) {
        return -MP_EINVAL;
    }

    if (want_read) {
        mode |= FA_READ;
    }
    if (want_write) {
        mode |= FA_WRITE;
    }

    if (append) {
        mode |= FA_OPEN_ALWAYS;
    } else if (truncate) {
        mode |= FA_CREATE_ALWAYS;
    } else if (create) {
        mode |= FA_OPEN_ALWAYS;
    } else {
        mode |= FA_OPEN_EXISTING;
    }

    *mode_out = mode;
    *append_out = append;
    return 0;
}

static int native_fat_check_open(const mp_native_fat_file_t *file) {
    return (file != NULL && file->open && file->mount != NULL) ? 0 : -MP_EBADF;
}

static int native_fat_fresult_to_errno(FRESULT res) {
    return res == FR_OK ? 0 : -fresult_to_errno_table[res];
}

int mp_native_fat_open_absolute(const char *path, int flags, mp_native_fat_file_t *out) {
    BYTE mode;
    bool append;
    const char *path_in_mount = NULL;

    if (out == NULL || path == NULL || path[0] != '/') {
        return -MP_EINVAL;
    }
    if (!native_fat_registry_inited) {
        return -MP_ENODEV;
    }

    int ret = native_fat_flags_to_mode(flags, &mode, &append);
    if (ret != 0) {
        return ret;
    }

    native_fat_registry_lock();
    mp_native_fat_mount_entry_t *entry = native_fat_find_entry_for_path(path, &path_in_mount);
    if (entry == NULL) {
        native_fat_registry_unlock();
        return -MP_ENODEV;
    }
    entry->active_handles += 1;
    native_fat_registry_unlock();

    memset(out, 0, sizeof(*out));
    FRESULT fres = f_open(&entry->mount->fatfs, &out->fp, path_in_mount, mode);
    if (fres != FR_OK) {
        native_fat_registry_lock();
        entry->active_handles -= 1;
        native_fat_registry_unlock();
        return native_fat_fresult_to_errno(fres);
    }

    if (append) {
        fres = f_lseek(&out->fp, f_size(&out->fp));
        if (fres != FR_OK) {
            f_close(&out->fp);
            native_fat_registry_lock();
            entry->active_handles -= 1;
            native_fat_registry_unlock();
            return native_fat_fresult_to_errno(fres);
        }
    }

    out->mount = entry;
    out->open = true;
    return 0;
}

int mp_native_fat_read(mp_native_fat_file_t *file, void *buf, size_t len, size_t *out_n) {
    UINT n = 0;

    int ret = native_fat_check_open(file);
    if (ret != 0) {
        return ret;
    }
    if (len > UINT_MAX) {
        return -MP_EINVAL;
    }

    FRESULT fres = f_read(&file->fp, buf, len, &n);
    if (out_n != NULL) {
        *out_n = n;
    }
    return native_fat_fresult_to_errno(fres);
}

int mp_native_fat_write(mp_native_fat_file_t *file, const void *buf, size_t len, size_t *out_n) {
    UINT n = 0;

    int ret = native_fat_check_open(file);
    if (ret != 0) {
        return ret;
    }
    if (len > UINT_MAX) {
        return -MP_EINVAL;
    }

    FRESULT fres = f_write(&file->fp, buf, len, &n);
    if (out_n != NULL) {
        *out_n = n;
    }
    if (fres != FR_OK) {
        return native_fat_fresult_to_errno(fres);
    }
    if (n != len) {
        return -MP_ENOSPC;
    }
    return 0;
}

int mp_native_fat_seek(mp_native_fat_file_t *file, FSIZE_t offset) {
    int ret = native_fat_check_open(file);
    if (ret != 0) {
        return ret;
    }
    return native_fat_fresult_to_errno(f_lseek(&file->fp, offset));
}

int mp_native_fat_tell(mp_native_fat_file_t *file, FSIZE_t *out_offset) {
    int ret = native_fat_check_open(file);
    if (ret != 0) {
        return ret;
    }
    if (out_offset == NULL) {
        return -MP_EINVAL;
    }
    *out_offset = f_tell(&file->fp);
    return 0;
}

int mp_native_fat_size(mp_native_fat_file_t *file, FSIZE_t *out_size) {
    int ret = native_fat_check_open(file);
    if (ret != 0) {
        return ret;
    }
    if (out_size == NULL) {
        return -MP_EINVAL;
    }
    *out_size = f_size(&file->fp);
    return 0;
}

int mp_native_fat_flush(mp_native_fat_file_t *file) {
    int ret = native_fat_check_open(file);
    if (ret != 0) {
        return ret;
    }
    return native_fat_fresult_to_errno(f_sync(&file->fp));
}

int mp_native_fat_close(mp_native_fat_file_t *file) {
    FRESULT fres = FR_OK;
    mp_native_fat_mount_entry_t *entry;

    int ret = native_fat_check_open(file);
    if (ret != 0) {
        return ret;
    }

    entry = file->mount;
    fres = f_close(&file->fp);
    file->open = false;
    file->mount = NULL;

    native_fat_registry_lock();
    if (entry->active_handles > 0) {
        entry->active_handles -= 1;
    }
    native_fat_registry_unlock();

    return native_fat_fresult_to_errno(fres);
}

int mp_native_fat_vfs_register(mp_vfs_mount_t *vfs) {
    if (!mp_obj_is_type(vfs->obj, &mp_fat_vfs_type)) {
        return 0;
    }

    int ret = native_fat_registry_ensure_inited();
    if (ret != 0) {
        return ret;
    }

    mp_native_fat_mount_entry_t *entry = malloc(sizeof(*entry));
    char *mount_path = malloc(vfs->len + 1);
    if (entry == NULL || mount_path == NULL) {
        free(entry);
        free(mount_path);
        return -MP_ENOMEM;
    }

    memcpy(mount_path, vfs->str, vfs->len);
    mount_path[vfs->len] = '\0';

    entry->mount = MP_OBJ_TO_PTR(vfs->obj);
    entry->mount_path = mount_path;
    entry->mount_len = vfs->len;
    entry->active_handles = 0;
    entry->mounted = true;

    native_fat_registry_lock();
    entry->next = native_fat_registry_head;
    native_fat_registry_head = entry;
    native_fat_registry_unlock();
    return 0;
}

int mp_native_fat_vfs_pre_umount(mp_vfs_mount_t *vfs) {
    if (!native_fat_registry_inited || !mp_obj_is_type(vfs->obj, &mp_fat_vfs_type)) {
        return 0;
    }

    native_fat_registry_lock();
    mp_native_fat_mount_entry_t *entry = native_fat_find_entry_for_mount(vfs);
    if (entry == NULL) {
        native_fat_registry_unlock();
        return 0;
    }
    if (entry->active_handles != 0) {
        native_fat_registry_unlock();
        return -MP_EBUSY;
    }
    entry->mounted = false;
    native_fat_registry_unlock();
    return 0;
}

void mp_native_fat_vfs_cancel_umount(mp_vfs_mount_t *vfs) {
    if (!native_fat_registry_inited || !mp_obj_is_type(vfs->obj, &mp_fat_vfs_type)) {
        return;
    }

    native_fat_registry_lock();
    mp_native_fat_mount_entry_t *entry = native_fat_find_entry_for_mount(vfs);
    if (entry != NULL) {
        entry->mounted = true;
    }
    native_fat_registry_unlock();
}

void mp_native_fat_vfs_unregister(mp_vfs_mount_t *vfs) {
    if (!native_fat_registry_inited || !mp_obj_is_type(vfs->obj, &mp_fat_vfs_type)) {
        return;
    }

    native_fat_registry_lock();
    for (mp_native_fat_mount_entry_t **entry = &native_fat_registry_head; *entry != NULL; entry = &(*entry)->next) {
        if ((*entry)->mount == MP_OBJ_TO_PTR(vfs->obj)) {
            mp_native_fat_mount_entry_t *found = *entry;
            *entry = found->next;
            native_fat_registry_unlock();
            free(found->mount_path);
            free(found);
            return;
        }
    }
    native_fat_registry_unlock();
}

#else

int mp_native_fat_open_absolute(const char *path, int flags, mp_native_fat_file_t *out) {
    (void)path;
    (void)flags;
    (void)out;
    return -MP_EOPNOTSUPP;
}

int mp_native_fat_read(mp_native_fat_file_t *file, void *buf, size_t len, size_t *out_n) {
    (void)file;
    (void)buf;
    (void)len;
    if (out_n != NULL) {
        *out_n = 0;
    }
    return -MP_EOPNOTSUPP;
}

int mp_native_fat_write(mp_native_fat_file_t *file, const void *buf, size_t len, size_t *out_n) {
    (void)file;
    (void)buf;
    (void)len;
    if (out_n != NULL) {
        *out_n = 0;
    }
    return -MP_EOPNOTSUPP;
}

int mp_native_fat_seek(mp_native_fat_file_t *file, FSIZE_t offset) {
    (void)file;
    (void)offset;
    return -MP_EOPNOTSUPP;
}

int mp_native_fat_tell(mp_native_fat_file_t *file, FSIZE_t *out_offset) {
    (void)file;
    if (out_offset != NULL) {
        *out_offset = 0;
    }
    return -MP_EOPNOTSUPP;
}

int mp_native_fat_size(mp_native_fat_file_t *file, FSIZE_t *out_size) {
    (void)file;
    if (out_size != NULL) {
        *out_size = 0;
    }
    return -MP_EOPNOTSUPP;
}

int mp_native_fat_flush(mp_native_fat_file_t *file) {
    (void)file;
    return -MP_EOPNOTSUPP;
}

int mp_native_fat_close(mp_native_fat_file_t *file) {
    (void)file;
    return -MP_EOPNOTSUPP;
}

int mp_native_fat_vfs_register(mp_vfs_mount_t *vfs) {
    (void)vfs;
    return 0;
}

int mp_native_fat_vfs_pre_umount(mp_vfs_mount_t *vfs) {
    (void)vfs;
    return 0;
}

void mp_native_fat_vfs_cancel_umount(mp_vfs_mount_t *vfs) {
    (void)vfs;
}

void mp_native_fat_vfs_unregister(mp_vfs_mount_t *vfs) {
    (void)vfs;
}

#endif

#endif // MICROPY_VFS && MICROPY_VFS_FAT
