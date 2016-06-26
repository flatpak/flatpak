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

/* Missing glibc definitions to access certain kernel APIs */

#if !HAVE_DECL_RENAMEAT2
#  ifndef __NR_renameat2
#    if defined __x86_64__
#      define __NR_renameat2 316
#    elif defined __arm__
#      define __NR_renameat2 382
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
