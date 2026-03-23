#ifndef MICROPY_INCLUDED_EXTMOD_VFS_FAT_NATIVE_H
#define MICROPY_INCLUDED_EXTMOD_VFS_FAT_NATIVE_H

#include <stdbool.h>
#include <stddef.h>

#include "py/mpconfig.h"

#if MICROPY_VFS && MICROPY_VFS_FAT

#include "extmod/vfs.h"
#include "extmod/vfs_fat.h"

typedef struct _mp_native_fat_mount_entry_t mp_native_fat_mount_entry_t;

typedef struct _mp_native_fat_file_t {
    FIL fp;
    mp_native_fat_mount_entry_t *mount;
    bool open;
} mp_native_fat_file_t;

#define MP_NATIVE_FAT_O_READ      (0x0001)
#define MP_NATIVE_FAT_O_WRITE     (0x0002)
#define MP_NATIVE_FAT_O_CREATE    (0x0004)
#define MP_NATIVE_FAT_O_TRUNCATE  (0x0008)
#define MP_NATIVE_FAT_O_APPEND    (0x0010)

int mp_native_fat_open_absolute(const char *path, int flags, mp_native_fat_file_t *out);
int mp_native_fat_read(mp_native_fat_file_t *file, void *buf, size_t len, size_t *out_n);
int mp_native_fat_write(mp_native_fat_file_t *file, const void *buf, size_t len, size_t *out_n);
int mp_native_fat_seek(mp_native_fat_file_t *file, FSIZE_t offset);
int mp_native_fat_tell(mp_native_fat_file_t *file, FSIZE_t *out_offset);
int mp_native_fat_size(mp_native_fat_file_t *file, FSIZE_t *out_size);
int mp_native_fat_flush(mp_native_fat_file_t *file);
int mp_native_fat_close(mp_native_fat_file_t *file);

int mp_native_fat_vfs_register(mp_vfs_mount_t *vfs);
int mp_native_fat_vfs_pre_umount(mp_vfs_mount_t *vfs);
void mp_native_fat_vfs_cancel_umount(mp_vfs_mount_t *vfs);
void mp_native_fat_vfs_unregister(mp_vfs_mount_t *vfs);

#endif // MICROPY_VFS && MICROPY_VFS_FAT

#endif // MICROPY_INCLUDED_EXTMOD_VFS_FAT_NATIVE_H
