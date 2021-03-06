/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 *  IDT handlers for the V86 monitor using KVM
 *  These just push the exception number and state, and then execute
 *  a common HLT which is trapped by KVM.
 */

	.section .text
	.code32

	.globl kvm_mon_start
kvm_mon_start:

        i = 0
        .rept 0x21
        .if i == 8 || (i >= 0xa && i <= 0xe) || i == 0x11 || i == 0x1e
        /* these exceptions already pushed an error code */
        nop
        nop
        .else
        /* push fake error code for consistent stack */
        pushl $0
        .endif
        pushl $i
        jmp kvm_mon_main
        i = i + 1
        .fill kvm_mon_start+10*i-., 1, 0x90
        .endr

kvm_mon_main:
        sub $0xc,%esp
        push %eax
        push %ebp
        push %edi
        push %esi
        push %edx
        push %ecx
        push %ebx
        mov %cr2,%eax
        push %eax
	.globl kvm_mon_hlt
kvm_mon_hlt:
        hlt
        pop %eax
        pop %ebx
        pop %ecx
        pop %edx
        pop %esi
        pop %edi
        pop %ebp
        pop %eax
        add $0x14,%esp
        iret

	.globl kvm_mon_end
kvm_mon_end:

#ifdef __ELF__
.section .note.GNU-stack,"",%progbits
#endif
