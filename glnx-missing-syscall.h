/***
  This file was originally part of systemd.

  Copyright 2010 Lennart Poettering
  Copyright 2016 Zbigniew Jędrzejewski-Szmek

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

static inline ssize_t copy_file_range(int fd_in, loff_t *off_in,
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
#endif
