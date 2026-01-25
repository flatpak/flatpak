/***
  This file was originally part of systemd.

  Copyright 2010 Lennart Poettering
  Copyright 2016 Zbigniew Jędrzejewski-Szmek
  SPDX-License-Identifier: LGPL-2.1-or-later

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

/* Missing glibc definitions to access certain kernel APIs.
   This file is last updated from systemd git:

   commit 71e5200f94b22589922704aa4abdf95d4fe2e528
   Author:     Daniel Mack <daniel@zonque.org>
   AuthorDate: Tue Oct 18 17:57:10 2016 +0200
   Commit:     Lennart Poettering <lennart@poettering.net>
   CommitDate: Fri Sep 22 15:24:54 2017 +0200

   Add abstraction model for BPF programs
*/

#include "libglnx-config.h"
#include <glib.h>
#include <stdint.h>

#if !HAVE_DECL_RENAMEAT2
#  ifndef __NR_renameat2
#    if defined __x86_64__
#      define __NR_renameat2 316
#    elif defined __arm__
#      define __NR_renameat2 382
#    elif defined __aarch64__
#      define __NR_renameat2 276
#    elif defined _MIPS_SIM
#      if _MIPS_SIM == _MIPS_SIM_ABI32
#        define __NR_renameat2 4351
#      endif
#      if _MIPS_SIM == _MIPS_SIM_NABI32
#        define __NR_renameat2 6315
#      endif
#      if _MIPS_SIM == _MIPS_SIM_ABI64
#        define __NR_renameat2 5311
#      endif
#    elif defined __i386__
#      define __NR_renameat2 353
#    elif defined __powerpc64__
#      define __NR_renameat2 357
#    elif defined __s390__ || defined __s390x__
#      define __NR_renameat2 347
#    elif defined __arc__
#      define __NR_renameat2 276
#    else
#      warning "__NR_renameat2 unknown for your architecture"
#    endif
#  endif

static inline int renameat2(int oldfd, const char *oldname, int newfd, const char *newname, unsigned flags) {
#  ifdef __NR_renameat2
        return syscall(__NR_renameat2, oldfd, oldname, newfd, newname, flags);
#  else
        errno = ENOSYS;
        return -1;
#  endif
}
#endif

#if !HAVE_DECL_MEMFD_CREATE
#  ifndef __NR_memfd_create
#    if defined __x86_64__
#      define __NR_memfd_create 319
#    elif defined __arm__
#      define __NR_memfd_create 385
#    elif defined __aarch64__
#      define __NR_memfd_create 279
#    elif defined __s390__
#      define __NR_memfd_create 350
#    elif defined _MIPS_SIM
#      if _MIPS_SIM == _MIPS_SIM_ABI32
#        define __NR_memfd_create 4354
#      endif
#      if _MIPS_SIM == _MIPS_SIM_NABI32
#        define __NR_memfd_create 6318
#      endif
#      if _MIPS_SIM == _MIPS_SIM_ABI64
#        define __NR_memfd_create 5314
#      endif
#    elif defined __i386__
#      define __NR_memfd_create 356
#    elif defined __arc__
#      define __NR_memfd_create 279
#    else
#      warning "__NR_memfd_create unknown for your architecture"
#    endif
#  endif

static inline int memfd_create(const char *name, unsigned int flags) {
#  ifdef __NR_memfd_create
        return syscall(__NR_memfd_create, name, flags);
#  else
        errno = ENOSYS;
        return -1;
#  endif
}
#endif

/* Copied from systemd git:
   commit 6bda23dd6aaba50cf8e3e6024248cf736cc443ca
   Author:     Yu Watanabe <watanabe.yu+github@gmail.com>
   AuthorDate: Thu Jul 27 20:22:54 2017 +0900
   Commit:     Zbigniew Jędrzejewski-Szmek <zbyszek@in.waw.pl>
   CommitDate: Thu Jul 27 07:22:54 2017 -0400
*/
#if !HAVE_DECL_COPY_FILE_RANGE
#  ifndef __NR_copy_file_range
#    if defined(__x86_64__)
#      define __NR_copy_file_range 326
#    elif defined(__i386__)
#      define __NR_copy_file_range 377
#    elif defined __s390__
#      define __NR_copy_file_range 375
#    elif defined __arm__
#      define __NR_copy_file_range 391
#    elif defined __aarch64__
#      define __NR_copy_file_range 285
#    elif defined __powerpc__
#      define __NR_copy_file_range 379
#    elif defined __arc__
#      define __NR_copy_file_range 285
#    else
#      warning "__NR_copy_file_range not defined for your architecture"
#    endif
#  endif

static inline ssize_t missing_copy_file_range(int fd_in, loff_t *off_in,
                                              int fd_out, loff_t *off_out,
                                              size_t len,
                                              unsigned int flags) {
#  ifdef __NR_copy_file_range
        return syscall(__NR_copy_file_range, fd_in, off_in, fd_out, off_out, len, flags);
#  else
        errno = ENOSYS;
        return -1;
#  endif
}

#  define copy_file_range missing_copy_file_range
#endif

#ifndef __IGNORE_close_range
#  if defined(__aarch64__)
#    define systemd_NR_close_range 436
#  elif defined(__alpha__)
#    define systemd_NR_close_range 546
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_close_range 436
#  elif defined(__arm__)
#    define systemd_NR_close_range 436
#  elif defined(__i386__)
#    define systemd_NR_close_range 436
#  elif defined(__ia64__)
#    define systemd_NR_close_range 1460
#  elif defined(__loongarch_lp64)
#    define systemd_NR_close_range 436
#  elif defined(__m68k__)
#    define systemd_NR_close_range 436
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_close_range 4436
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_close_range 6436
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_close_range 5436
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_close_range 436
#  elif defined(__powerpc__)
#    define systemd_NR_close_range 436
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_close_range 436
#    elif __riscv_xlen == 64
#      define systemd_NR_close_range 436
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_close_range 436
#  elif defined(__sparc__)
#    define systemd_NR_close_range 436
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_close_range (436 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_close_range 436
#    endif
#  elif !defined(missing_arch_template)
#    warning "close_range() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_close_range && __NR_close_range >= 0
#    if defined systemd_NR_close_range
G_STATIC_ASSERT(__NR_close_range == systemd_NR_close_range);
#    endif
#  else
#    if defined __NR_close_range
#      undef __NR_close_range
#    endif
#    if defined systemd_NR_close_range && systemd_NR_close_range >= 0
#      define __NR_close_range systemd_NR_close_range
#    endif
#  endif
#endif

#if !defined(HAVE_CLOSE_RANGE) && defined(__NR_close_range)
static inline int
inline_close_range (unsigned int low,
                    unsigned int high,
                    int flags)
{
  return syscall (__NR_close_range, low, high, flags);
}
#define close_range(low, high, flags) inline_close_range(low, high, flags)
#define HAVE_CLOSE_RANGE
#endif

#ifndef __IGNORE_statx
#  if defined(__aarch64__)
#    define systemd_NR_statx 291
#  elif defined(__alpha__)
#    define systemd_NR_statx 522
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_statx 291
#  elif defined(__arm__)
#    define systemd_NR_statx 397
#  elif defined(__i386__)
#    define systemd_NR_statx 383
#  elif defined(__ia64__)
#    define systemd_NR_statx 1350
#  elif defined(__loongarch_lp64)
#    define systemd_NR_statx 291
#  elif defined(__m68k__)
#    define systemd_NR_statx 379
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_statx 4366
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_statx 6330
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_statx 5326
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_statx 349
#  elif defined(__powerpc__)
#    define systemd_NR_statx 383
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_statx 291
#    elif __riscv_xlen == 64
#      define systemd_NR_statx 291
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_statx 379
#  elif defined(__sparc__)
#    define systemd_NR_statx 360
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_statx (332 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_statx 332
#    endif
#  elif !defined(missing_arch_template)
#    warning "statx() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_statx && __NR_statx >= 0
#    if defined systemd_NR_statx
G_STATIC_ASSERT (__NR_statx == systemd_NR_statx);
#    endif
#  else
#    if defined __NR_statx
#      undef __NR_statx
#    endif
#    if defined systemd_NR_statx && systemd_NR_statx >= 0
#      define __NR_statx systemd_NR_statx
#    endif
#  endif
#endif

#if !defined(HAVE_GLNX_STATX) && defined(__NR_statx)
#define GLNX_STATX_TYPE              0x00000001U     /* Want/got stx_mode & S_IFMT */
#define GLNX_STATX_MODE              0x00000002U     /* Want/got stx_mode & ~S_IFMT */
#define GLNX_STATX_NLINK             0x00000004U     /* Want/got stx_nlink */
#define GLNX_STATX_UID               0x00000008U     /* Want/got stx_uid */
#define GLNX_STATX_GID               0x00000010U     /* Want/got stx_gid */
#define GLNX_STATX_ATIME             0x00000020U     /* Want/got stx_atime */
#define GLNX_STATX_MTIME             0x00000040U     /* Want/got stx_mtime */
#define GLNX_STATX_CTIME             0x00000080U     /* Want/got stx_ctime */
#define GLNX_STATX_INO               0x00000100U     /* Want/got stx_ino */
#define GLNX_STATX_SIZE              0x00000200U     /* Want/got stx_size */
#define GLNX_STATX_BLOCKS            0x00000400U     /* Want/got stx_blocks */
#define GLNX_STATX_BASIC_STATS       0x000007ffU     /* The stuff in the normal stat struct */
#define GLNX_STATX_BTIME             0x00000800U     /* Want/got stx_btime */
#define GLNX_STATX_MNT_ID            0x00001000U     /* Got stx_mnt_id */
#define GLNX_STATX_DIOALIGN          0x00002000U     /* Want/got direct I/O alignment info */
#define GLNX_STATX_MNT_ID_UNIQUE     0x00004000U     /* Want/got extended stx_mount_id */
#define GLNX_STATX_SUBVOL            0x00008000U     /* Want/got stx_subvol */
#define GLNX_STATX_WRITE_ATOMIC      0x00010000U     /* Want/got atomic_write_* fields */
#define GLNX_STATX_DIO_READ_ALIGN    0x00020000U     /* Want/got dio read alignment info */
#define GLNX_STATX__RESERVED         0x80000000U     /* Reserved for future struct statx expansion */

struct glnx_statx_timestamp
{
  int64_t tv_sec;
  uint32_t tv_nsec;
  int32_t __reserved;
};

struct glnx_statx
{
  uint32_t stx_mask;
  uint32_t stx_blksize;
  uint64_t stx_attributes;
  uint32_t stx_nlink;
  uint32_t stx_uid;
  uint32_t stx_gid;
  uint16_t stx_mode;
  uint16_t __spare0[1];
  uint64_t stx_ino;
  uint64_t stx_size;
  uint64_t stx_blocks;
  uint64_t stx_attributes_mask;
  struct glnx_statx_timestamp stx_atime;
  struct glnx_statx_timestamp stx_btime;
  struct glnx_statx_timestamp stx_ctime;
  struct glnx_statx_timestamp stx_mtime;
  uint32_t stx_rdev_major;
  uint32_t stx_rdev_minor;
  uint32_t stx_dev_major;
  uint32_t stx_dev_minor;
  uint64_t stx_mnt_id;
  uint32_t stx_dio_mem_align;
  uint32_t stx_dio_offset_align;
  uint64_t stx_subvol;
  uint32_t stx_atomic_write_unit_min;
  uint32_t stx_atomic_write_unit_max;
  uint32_t stx_atomic_write_segments_max;
  uint32_t stx_dio_read_offset_align;
  uint32_t stx_atomic_write_unit_max_opt;
  uint32_t	__spare2[1];
  uint64_t	__spare3[8];
};

static inline int
glnx_statx_syscall (int                dfd,
                    const char        *filename,
                    unsigned           flags,
                    unsigned int       mask,
                    struct glnx_statx *buf)
{
	memset (buf, 0xbf, sizeof (*buf));
	return syscall (__NR_statx, dfd, filename, flags, mask, buf);
  return 0;
}

#define HAVE_GLNX_STATX
#endif

/* Copied from systemd git: ff83795469 ("boot: Improve log message")
 * - open_tree
 * - openat2
 */

#ifndef __IGNORE_open_tree
#  if defined(__aarch64__)
#    define systemd_NR_open_tree 428
#  elif defined(__alpha__)
#    define systemd_NR_open_tree 538
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_open_tree 428
#  elif defined(__arm__)
#    define systemd_NR_open_tree 428
#  elif defined(__i386__)
#    define systemd_NR_open_tree 428
#  elif defined(__ia64__)
#    define systemd_NR_open_tree 1452
#  elif defined(__loongarch_lp64)
#    define systemd_NR_open_tree 428
#  elif defined(__m68k__)
#    define systemd_NR_open_tree 428
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_open_tree 4428
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_open_tree 6428
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_open_tree 5428
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_open_tree 428
#  elif defined(__powerpc__)
#    define systemd_NR_open_tree 428
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_open_tree 428
#    elif __riscv_xlen == 64
#      define systemd_NR_open_tree 428
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_open_tree 428
#  elif defined(__sparc__)
#    define systemd_NR_open_tree 428
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_open_tree (428 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_open_tree 428
#    endif
#  elif !defined(missing_arch_template)
#    warning "open_tree() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_open_tree && __NR_open_tree >= 0
#    if defined systemd_NR_open_tree
G_STATIC_ASSERT (__NR_open_tree == systemd_NR_open_tree);
#    endif
#  else
#    if defined __NR_open_tree
#      undef __NR_open_tree
#    endif
#    if defined systemd_NR_open_tree && systemd_NR_open_tree >= 0
#      define __NR_open_tree systemd_NR_open_tree
#    endif
#  endif
#endif

#if !defined(HAVE_OPEN_TREE) && defined(__NR_open_tree)
#ifndef OPEN_TREE_CLONE
#define OPEN_TREE_CLONE 1
#endif

#ifndef OPEN_TREE_CLOEXEC
#define OPEN_TREE_CLOEXEC O_CLOEXEC
#endif

static inline int
inline_open_tree (int         dfd,
                  const char *filename,
                  unsigned    flags)
{
  return syscall(__NR_open_tree, dfd, filename, flags);
}
#define open_tree inline_open_tree
#define HAVE_OPEN_TREE
#endif

#ifndef __IGNORE_openat2
#  if defined(__aarch64__)
#    define systemd_NR_openat2 437
#  elif defined(__alpha__)
#    define systemd_NR_openat2 547
#  elif defined(__arc__) || defined(__tilegx__)
#    define systemd_NR_openat2 437
#  elif defined(__arm__)
#    define systemd_NR_openat2 437
#  elif defined(__i386__)
#    define systemd_NR_openat2 437
#  elif defined(__ia64__)
#    define systemd_NR_openat2 1461
#  elif defined(__loongarch_lp64)
#    define systemd_NR_openat2 437
#  elif defined(__m68k__)
#    define systemd_NR_openat2 437
#  elif defined(_MIPS_SIM)
#    if _MIPS_SIM == _MIPS_SIM_ABI32
#      define systemd_NR_openat2 4437
#    elif _MIPS_SIM == _MIPS_SIM_NABI32
#      define systemd_NR_openat2 6437
#    elif _MIPS_SIM == _MIPS_SIM_ABI64
#      define systemd_NR_openat2 5437
#    else
#      error "Unknown MIPS ABI"
#    endif
#  elif defined(__hppa__)
#    define systemd_NR_openat2 437
#  elif defined(__powerpc__)
#    define systemd_NR_openat2 437
#  elif defined(__riscv)
#    if __riscv_xlen == 32
#      define systemd_NR_openat2 437
#    elif __riscv_xlen == 64
#      define systemd_NR_openat2 437
#    else
#      error "Unknown RISC-V ABI"
#    endif
#  elif defined(__s390__)
#    define systemd_NR_openat2 437
#  elif defined(__sparc__)
#    define systemd_NR_openat2 437
#  elif defined(__x86_64__)
#    if defined(__ILP32__)
#      define systemd_NR_openat2 (437 | /* __X32_SYSCALL_BIT */ 0x40000000)
#    else
#      define systemd_NR_openat2 437
#    endif
#  elif !defined(missing_arch_template)
#    warning "openat2() syscall number is unknown for your architecture"
#  endif

/* may be an (invalid) negative number due to libseccomp, see PR 13319 */
#  if defined __NR_openat2 && __NR_openat2 >= 0
#    if defined systemd_NR_openat2
G_STATIC_ASSERT (__NR_openat2 == systemd_NR_openat2);
#    endif
#  else
#    if defined __NR_openat2
#      undef __NR_openat2
#    endif
#    if defined systemd_NR_openat2 && systemd_NR_openat2 >= 0
#      define __NR_openat2 systemd_NR_openat2
#    endif
#  endif
#endif

#if !defined(HAVE_OPENAT2) && defined(__NR_openat2)
#ifndef RESOLVE_NO_XDEV
#define RESOLVE_NO_XDEV 0x01
#endif

#ifndef RESOLVE_NO_MAGICLINKS
#define RESOLVE_NO_MAGICLINKS 0x02
#endif

#ifndef RESOLVE_NO_SYMLINKS
#define RESOLVE_NO_SYMLINKS 0x04
#endif

#ifndef RESOLVE_BENEATH
#define RESOLVE_BENEATH 0x08
#endif

#ifndef RESOLVE_IN_ROOT
#define RESOLVE_IN_ROOT 0x10
#endif

#ifndef RESOLVE_CACHED
#define RESOLVE_CACHED 0x20
#endif

struct inline_open_how {
        uint64_t flags;
        uint64_t mode;
        uint64_t resolve;
};
#define open_how inline_open_how

static inline int
inline_openat2 (int         dfd,
                const char *filename,
                void       *buffer,
                size_t      size)
{
  return syscall(__NR_openat2, dfd, filename, buffer, size);
}
#define openat2 inline_openat2
#define HAVE_OPENAT2
#endif
