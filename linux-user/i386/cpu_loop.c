/*
 *  qemu user cpu loop
 *
 *  Copyright (c) 2003-2008 Fabrice Bellard
 *
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
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu.h"
#include "cpu_loop-common.h"
#include "latx-options.h"

/***********************************************************/
/* CPUX86 core interface */

uint64_t cpu_get_tsc(CPUX86State *env)
{
    return cpu_get_host_ticks();
}

static void write_dt(void *ptr, unsigned long addr, unsigned long limit,
              int flags)
{
    unsigned int e1, e2;
    uint32_t *p;
    e1 = (addr << 16) | (limit & 0xffff);
    e2 = ((addr >> 16) & 0xff) | (addr & 0xff000000) | (limit & 0x000f0000);
    e2 |= flags;
    p = ptr;
    p[0] = tswap32(e1);
    p[1] = tswap32(e2);
}

static uint64_t *idt_table;
#ifdef TARGET_X86_64
static void set_gate64(void *ptr, unsigned int type, unsigned int dpl,
                       uint64_t addr, unsigned int sel)
{
    uint32_t *p, e1, e2;
    e1 = (addr & 0xffff) | (sel << 16);
    e2 = (addr & 0xffff0000) | 0x8000 | (dpl << 13) | (type << 8);
    p = ptr;
    p[0] = tswap32(e1);
    p[1] = tswap32(e2);
    p[2] = tswap32(addr >> 32);
    p[3] = 0;
}
/* only dpl matters as we do only user space emulation */
static void set_idt(int n, unsigned int dpl)
{
    set_gate64(idt_table + n * 2, 0, dpl, 0, 0);
}
#else
static void set_gate(void *ptr, unsigned int type, unsigned int dpl,
                     uint32_t addr, unsigned int sel)
{
    uint32_t *p, e1, e2;
    e1 = (addr & 0xffff) | (sel << 16);
    e2 = (addr & 0xffff0000) | 0x8000 | (dpl << 13) | (type << 8);
    p = ptr;
    p[0] = tswap32(e1);
    p[1] = tswap32(e2);
}

/* only dpl matters as we do only user space emulation */
static void set_idt(int n, unsigned int dpl)
{
    set_gate(idt_table + n, 0, dpl, 0, 0);
}
#endif

static void gen_signal(CPUX86State *env, int sig, int code, abi_ptr addr)
{
    target_siginfo_t info = {
        .si_signo = sig,
        .si_code = code,
        ._sifields._sigfault._addr = addr
    };

    queue_signal(env, info.si_signo, QEMU_SI_FAULT, &info);
}

#ifdef TARGET_X86_64
static bool write_ok_or_segv(CPUX86State *env, abi_ptr addr, size_t len)
{
    /*
     * For all the vsyscalls, NULL means "don't write anything" not
     * "write it at address 0".
     */
    if (addr == 0 || access_ok(env_cpu(env), VERIFY_WRITE, addr, len)) {
        return true;
    }

    env->error_code = PG_ERROR_W_MASK | PG_ERROR_U_MASK;
    gen_signal(env, TARGET_SIGSEGV, TARGET_SEGV_MAPERR, addr);
    return false;
}

/*
 * Since v3.1, the kernel traps and emulates the vsyscall page.
 * Entry points other than the official generate SIGSEGV.
 */
static void emulate_vsyscall(CPUX86State *env)
{
    int syscall;
    abi_ulong ret;
    uint64_t caller;

    /*
     * Validate the entry point.  We have already validated the page
     * during translation to get here; now verify the offset.
     */
    switch (env->eip & ~TARGET_PAGE_MASK) {
    case 0x000:
        syscall = TARGET_NR_gettimeofday;
        break;
    case 0x400:
        syscall = TARGET_NR_time;
        break;
    case 0x800:
        syscall = TARGET_NR_getcpu;
        break;
    default:
        goto sigsegv;
    }

    /*
     * Validate the return address.
     * Note that the kernel treats this the same as an invalid entry point.
     */
    if (get_user_u64(caller, env->regs[R_ESP])) {
        goto sigsegv;
    }

    /*
     * Validate the the pointer arguments.
     */
    switch (syscall) {
    case TARGET_NR_gettimeofday:
        if (!write_ok_or_segv(env, env->regs[R_EDI],
                              sizeof(struct target_timeval)) ||
            !write_ok_or_segv(env, env->regs[R_ESI],
                              sizeof(struct target_timezone))) {
            return;
        }
        break;
    case TARGET_NR_time:
        if (!write_ok_or_segv(env, env->regs[R_EDI], sizeof(abi_long))) {
            return;
        }
        break;
    case TARGET_NR_getcpu:
        if (!write_ok_or_segv(env, env->regs[R_EDI], sizeof(uint32_t)) ||
            !write_ok_or_segv(env, env->regs[R_ESI], sizeof(uint32_t))) {
            return;
        }
        break;
    default:
        g_assert_not_reached();
    }

    /*
     * Perform the syscall.  None of the vsyscalls should need restarting.
     */
    ret = do_syscall(env, syscall, env->regs[R_EDI], env->regs[R_ESI],
                     env->regs[R_EDX], env->regs[10], env->regs[8],
                     env->regs[9], 0, 0);
    g_assert(ret != -TARGET_ERESTARTSYS);
    g_assert(ret != -TARGET_QEMU_ESIGRETURN);
    if (ret == -TARGET_EFAULT) {
        goto sigsegv;
    }
    env->regs[R_EAX] = ret;

    /* Emulate a ret instruction to leave the vsyscall page.  */
    env->eip = caller;
    env->regs[R_ESP] += 8;
    return;

 sigsegv:
    /* Like force_sig(SIGSEGV).  */
    gen_signal(env, TARGET_SIGSEGV, TARGET_SI_KERNEL, 0);
}
#endif
#ifdef TARGET_X86_64
#include "syscall_64_nr.h"
#include "syscall_target_32_nr.h"
int syscall_64_to_32[TARGET32_TARGET_NR_LATX_LAST + 1] = {0};
#include "syscall_64_to_32_map.h"
#include "lsenv.h"
#endif

void cpu_loop(CPUX86State *env)
{
    CPUState *cs = env_cpu(env);
    int trapnr;
    abi_ulong pc;
    abi_ulong ret;

 #ifdef TARGET_X86_64
     INIT_SYSCALL_64_TO_32();
 #endif
    for(;;) {
        cpu_exec_start(cs);
        trapnr = cpu_exec(cs);
        cpu_exec_end(cs);
        process_queued_cpu_work(cs);
#if defined(CONFIG_LATX_KZT)
        if(option_kzt && trapnr == 0xCC)
            break;
#endif
        switch(trapnr) {
        case 0x80:
            /* linux syscall from int $0x80 */
            ret = do_syscall(env,
#ifdef TARGET_X86_64
                             syscall_64_to_32[env->regs[R_EAX]],
#else
                             env->regs[R_EAX],
#endif
                             env->regs[R_EBX],
                             env->regs[R_ECX],
                             env->regs[R_EDX],
                             env->regs[R_ESI],
                             env->regs[R_EDI],
                             env->regs[R_EBP],
                             0, 0);
            if (ret == -TARGET_ERESTARTSYS) {
                env->eip -= 2;
            } else if (ret != -TARGET_QEMU_ESIGRETURN) {
                env->regs[R_EAX] = ret;
            }
            break;
#ifndef TARGET_ABI32
        case EXCP_SYSCALL:
            /* linux syscall from syscall instruction */
            ret = do_syscall(env,
                             env->regs[R_EAX],
                             env->regs[R_EDI],
                             env->regs[R_ESI],
                             env->regs[R_EDX],
                             env->regs[10],
                             env->regs[8],
                             env->regs[9],
                             0, 0);
            if (ret == -TARGET_ERESTARTSYS) {
                env->eip -= 2;
            } else if (ret != -TARGET_QEMU_ESIGRETURN) {
                env->regs[R_EAX] = ret;
            }
            break;
#endif
#ifdef TARGET_X86_64
        case EXCP_VSYSCALL:
            emulate_vsyscall(env);
            break;
#endif
        case EXCP0B_NOSEG:
            /*
             * For pulseaudio sigbus-test, addr need to be report
             * to guest for signal handler usage.
             */
            gen_signal(env, TARGET_SIGBUS, TARGET_SI_KERNEL, env->cr[2]);
            break;
        case EXCP0C_STACK:
            gen_signal(env, TARGET_SIGBUS, TARGET_SI_KERNEL, 0);
            break;
        case EXCP0D_GPF:
            /* XXX: potential problem if ABI32 */
#ifndef TARGET_X86_64
            if (env->eflags & VM_MASK) {
                handle_vm86_fault(env);
                break;
            }
#endif
            gen_signal(env, TARGET_SIGSEGV, TARGET_SI_KERNEL, 0);
            break;
        case EXCP0E_PAGE:
            gen_signal(env, TARGET_SIGSEGV,
                       (env->error_code & 1 ?
                        TARGET_SEGV_ACCERR : TARGET_SEGV_MAPERR),
                       env->cr[2]);
            break;
        case EXCP00_DIVZ:
#ifndef TARGET_X86_64
            if (env->eflags & VM_MASK) {
                handle_vm86_trap(env, trapnr);
                break;
            }
#endif
            gen_signal(env, TARGET_SIGFPE, TARGET_FPE_INTDIV, env->eip);
            break;
        case EXCP10_COPR:
        {
        /* EXCP10_COPR is raised by soft float */
#define FPUS_IE (1 << 0)
#define FPUS_ZE (1 << 2)
#define FPUS_OE (1 << 3)
#define FPUS_UE (1 << 4)
#define FPUS_PE (1 << 5)
            int si_code = TARGET_FPE_FLTUNK;
            if (env->fpus & FPUS_IE) {
                si_code = TARGET_FPE_FLTINV;
            } else if (env->fpus & FPUS_ZE) {
                si_code = TARGET_FPE_FLTDIV;
            } else if (env->fpus & FPUS_OE) {
                si_code = TARGET_FPE_FLTOVF;
            } else if (env->fpus & FPUS_UE) {
                si_code = TARGET_FPE_FLTUND;
            } else if (env->fpus & FPUS_PE) {
                si_code = TARGET_FPE_FLTRES;
            }
            gen_signal(env, TARGET_SIGFPE, si_code, env->eip);
        }
            break;
        case EXCP01_DB:
        case EXCP03_INT3:
#ifndef TARGET_X86_64
            if (env->eflags & VM_MASK) {
                handle_vm86_trap(env, trapnr);
                break;
            }
#endif
            if (trapnr == EXCP01_DB) {
                gen_signal(env, TARGET_SIGTRAP, TARGET_TRAP_BRKPT, env->eip);
            } else {
                gen_signal(env, TARGET_SIGTRAP, TARGET_SI_KERNEL, 0);
            }
            break;
        case EXCP04_INTO:
        case EXCP05_BOUND:
#ifndef TARGET_X86_64
            if (env->eflags & VM_MASK) {
                handle_vm86_trap(env, trapnr);
                break;
            }
#endif
            gen_signal(env, TARGET_SIGSEGV, TARGET_SI_KERNEL, 0);
            break;
        case EXCP06_ILLOP:
            gen_signal(env, TARGET_SIGILL, TARGET_ILL_ILLOPN, env->eip);
            break;
        case EXCP_INTERRUPT:
            /* just indicate that signals should be handled asap */
            break;
        case EXCP_DEBUG:
            gen_signal(env, TARGET_SIGTRAP, TARGET_TRAP_BRKPT, 0);
            break;
        case EXCP_ATOMIC:
            cpu_exec_step_atomic(cs);
            break;
        default:
            pc = env->segs[R_CS].base + env->eip;
            EXCP_DUMP(env, "qemu: 0x%08lx: unhandled CPU exception 0x%x - aborting\n",
                      (long)pc, trapnr);
            abort();
        }
        process_pending_signals(env);
    }
}

void target_cpu_copy_regs(CPUArchState *env, struct target_pt_regs *regs)
{
    env->cr[0] = CR0_PG_MASK | CR0_WP_MASK | CR0_PE_MASK | CR0_NE_MASK;
    env->hflags |= HF_PE_MASK | HF_CPL_MASK;
    if (env->features[FEAT_1_EDX] & CPUID_SSE) {
        env->cr[4] |= CR4_OSFXSR_MASK;
        env->hflags |= HF_OSFXSR_MASK;
    }
#ifndef TARGET_ABI32
    /* enable 64 bit mode if possible */
    if (!(env->features[FEAT_8000_0001_EDX] & CPUID_EXT2_LM)) {
        fprintf(stderr, "The selected x86 CPU does not support 64 bit mode\n");
        exit(EXIT_FAILURE);
    }
    env->cr[4] |= CR4_PAE_MASK;
    env->efer |= MSR_EFER_LMA | MSR_EFER_LME;
    env->hflags |= HF_LMA_MASK;
#endif

    /* flags setup : we activate the IRQs by default as in user mode */
    env->eflags |= IF_MASK;

    /* linux register setup */
#ifndef TARGET_ABI32
    env->regs[R_EAX] = regs->rax;
    env->regs[R_EBX] = regs->rbx;
    env->regs[R_ECX] = regs->rcx;
    env->regs[R_EDX] = regs->rdx;
    env->regs[R_ESI] = regs->rsi;
    env->regs[R_EDI] = regs->rdi;
    env->regs[R_EBP] = regs->rbp;
    env->regs[R_ESP] = regs->rsp;
    env->eip = regs->rip;
#else
    env->regs[R_EAX] = regs->eax;
    env->regs[R_EBX] = regs->ebx;
    env->regs[R_ECX] = regs->ecx;
    env->regs[R_EDX] = regs->edx;
    env->regs[R_ESI] = regs->esi;
    env->regs[R_EDI] = regs->edi;
    env->regs[R_EBP] = regs->ebp;
    env->regs[R_ESP] = regs->esp;
    env->eip = regs->eip;
#endif

    /* linux interrupt setup */
#ifndef TARGET_ABI32
    env->idt.limit = 511;
#else
    env->idt.limit = 255;
#endif
    env->idt.base = target_mmap(0, sizeof(uint64_t) * (env->idt.limit + 1),
                                PROT_READ|PROT_WRITE,
                                MAP_ANONYMOUS|MAP_PRIVATE, -1, 0, 0);
    idt_table = g2h_untagged(env->idt.base);
    set_idt(0, 0);
    set_idt(1, 0);
    set_idt(2, 0);
    set_idt(3, 3);
    set_idt(4, 3);
    set_idt(5, 0);
    set_idt(6, 0);
    set_idt(7, 0);
    set_idt(8, 0);
    set_idt(9, 0);
    set_idt(10, 0);
    set_idt(11, 0);
    set_idt(12, 0);
    set_idt(13, 0);
    set_idt(14, 0);
    set_idt(15, 0);
    set_idt(16, 0);
    set_idt(17, 0);
    set_idt(18, 0);
    set_idt(19, 0);
    set_idt(0x80, 3);

    /* linux segment setup */
    {
        uint64_t *gdt_table;
        env->gdt.base = target_mmap(0, sizeof(uint64_t) * TARGET_GDT_ENTRIES,
                                    PROT_READ|PROT_WRITE,
                                    MAP_ANONYMOUS|MAP_PRIVATE, -1, 0, 0);
        env->gdt.limit = sizeof(uint64_t) * TARGET_GDT_ENTRIES - 1;
        gdt_table = g2h_untagged(env->gdt.base);
#ifdef TARGET_ABI32
        write_dt(&gdt_table[__USER_CS >> 3], 0, 0xfffff,
                 DESC_G_MASK | DESC_B_MASK | DESC_P_MASK | DESC_S_MASK |
                 (3 << DESC_DPL_SHIFT) | (0xa << DESC_TYPE_SHIFT));
#else
        /* 32 bit code segment */
        write_dt(&gdt_table[__USER32_CS >> 3], 0, 0xfffff,
                 DESC_G_MASK | DESC_B_MASK | DESC_P_MASK | DESC_S_MASK |
                 (3 << DESC_DPL_SHIFT) | (0xa << DESC_TYPE_SHIFT));
        /* 64 bit code segment */
        write_dt(&gdt_table[__USER_CS >> 3], 0, 0xfffff,
                 DESC_G_MASK | DESC_B_MASK | DESC_P_MASK | DESC_S_MASK |
                 DESC_L_MASK |
                 (3 << DESC_DPL_SHIFT) | (0xa << DESC_TYPE_SHIFT));
#endif
        write_dt(&gdt_table[__USER_DS >> 3], 0, 0xfffff,
                 DESC_G_MASK | DESC_B_MASK | DESC_P_MASK | DESC_S_MASK |
                 (3 << DESC_DPL_SHIFT) | (0x2 << DESC_TYPE_SHIFT));
    }
    cpu_x86_load_seg(env, R_CS, __USER_CS);
    cpu_x86_load_seg(env, R_SS, __USER_DS);
#ifdef TARGET_ABI32
    cpu_x86_load_seg(env, R_DS, __USER_DS);
    cpu_x86_load_seg(env, R_ES, __USER_DS);
    cpu_x86_load_seg(env, R_FS, __USER_DS);
    cpu_x86_load_seg(env, R_GS, __USER_DS);
    /* This hack makes Wine work... */
    env->segs[R_FS].selector = 0;
#else
    cpu_x86_load_seg(env, R_DS, 0);
    cpu_x86_load_seg(env, R_ES, 0);
    cpu_x86_load_seg(env, R_FS, 0);
    cpu_x86_load_seg(env, R_GS, 0);
#endif
}
