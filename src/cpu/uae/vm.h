/*
  Hatari - uae/vm.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Host virtual memory functions of the UAE CPU core (written anew for
  Hatari). Hatari does not build the code that uses them (the JIT and the
  x87 host FPU), so only the declarations are here.
*/

#ifndef UAE_VM_H
#define UAE_VM_H

#include "uae/types.h"

/* Protection: use only these combinations */
#define UAE_VM_NO_ACCESS		0
#define UAE_VM_READ			1
#define UAE_VM_WRITE			2
#define UAE_VM_EXECUTE			4
#define UAE_VM_READ_WRITE		(UAE_VM_READ | UAE_VM_WRITE)
#define UAE_VM_READ_EXECUTE		(UAE_VM_READ | UAE_VM_EXECUTE)
#define UAE_VM_READ_WRITE_EXECUTE	(UAE_VM_READ | UAE_VM_WRITE | UAE_VM_EXECUTE)

/* Flags */
#define UAE_VM_32BIT		(1 << 8)	/* below 4 GB */
#define UAE_VM_WRITE_WATCH	(1 << 9)

#define UAE_VM_ALLOC_FAILED	NULL

void *uae_vm_alloc(uae_u32 size, int flags, int protect);
bool uae_vm_protect(void *address, int size, int protect);
bool uae_vm_free(void *address, int size);
void *uae_vm_reserve(uae_u32 size, int flags);
void *uae_vm_reserve_fixed(void *address, uae_u32 size, int flags);
void *uae_vm_commit(void *address, uae_u32 size, int protect);
bool uae_vm_decommit(void *address, uae_u32 size);
int uae_vm_page_size(void);

#endif /* UAE_VM_H */
