/* The LibVMI Library is an introspection library that simplifies access to
 * memory in a target virtual machine or in a file containing a dump of
 * a system's physical memory.  LibVMI is based on the XenAccess Library.
 *
 * Copyright 2011 Sandia Corporation. Under the terms of Contract
 * DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government
 * retains certain rights in this software.
 *
 * Author: Bryan D. Payne (bdpayne@acm.org)
 * Author: Tamas K Lengyel (tamas.lengyel@zentific.com)
 *
 * This file is part of LibVMI.
 *
 * LibVMI is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * LibVMI is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with LibVMI.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <glib.h>
#include <math.h>
#include <glib/gstdio.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include "private.h"
#include "driver/driver_wrapper.h"
#include "driver/memory_cache.h"
#include "driver/kvm/kvm.h"
#include "driver/kvm/kvm_private.h"

#include <sys/time.h>
#include "driver/kvm/include/kvmi/libkvmi.h"

static uint32_t
translate_msr_index(int index, int *err) {
    *err = 0;
    switch (index) {
    case MSR_EFER:                  return 0xc0000080;
    case MSR_STAR:                  return 0xc0000081;
    case MSR_LSTAR:                 return 0xc0000082;
    case MSR_CSTAR:                 return 0xc0000083;
    case MSR_SYSCALL_MASK:          return 0xc0000084;
    case MSR_SHADOW_GS_BASE:        return 0xc0000102;
    case MSR_TSC_AUX:               return 0xc0000103;
    case MSR_MTRRfix64K_00000:      return 0x00000250;
    case MSR_MTRRfix16K_80000:      return 0x00000258;
    case MSR_MTRRfix16K_A0000:      return 0x00000259;
    case MSR_MTRRfix4K_C0000:       return 0x00000268;
    case MSR_MTRRfix4K_C8000:       return 0x00000269;
    case MSR_MTRRfix4K_D0000:       return 0x0000026a;
    case MSR_MTRRfix4K_D8000:       return 0x0000026b;
    case MSR_MTRRfix4K_E0000:       return 0x0000026c;
    case MSR_MTRRfix4K_E8000:       return 0x0000026d;
    case MSR_MTRRfix4K_F0000:       return 0x0000026e;
    case MSR_MTRRfix4K_F8000:       return 0x0000026f;
    case MSR_MTRRdefType:           return 0x000002ff;
    case MSR_IA32_MC0_CTL:          return 0x00000400;
    case MSR_IA32_MC0_STATUS:       return 0x00000401;
    case MSR_IA32_MC0_ADDR:         return 0x00000402;
    case MSR_IA32_MC0_MISC:         return 0x00000403;
    case MSR_IA32_MC1_CTL:          return 0x00000404;
    case MSR_IA32_MC0_CTL2:         return 0x00000280;
    case MSR_AMD_PATCHLEVEL:        return 0x0000008b;
    case MSR_AMD64_TSC_RATIO:       return 0xc0000104;
    case MSR_IA32_P5_MC_ADDR:       return 0x00000000;
    case MSR_IA32_P5_MC_TYPE:       return 0x00000001;
    case MSR_IA32_TSC:              return 0x00000010;
    case MSR_IA32_PLATFORM_ID:      return 0x00000017;
    case MSR_IA32_EBL_CR_POWERON:   return 0x0000002a;
    case MSR_IA32_EBC_FREQUENCY_ID: return 0x0000002c;
    case MSR_IA32_FEATURE_CONTROL:  return 0x0000003a;
    case MSR_IA32_SYSENTER_CS:      return 0x00000174;
    case MSR_IA32_SYSENTER_ESP:     return 0x00000175;
    case MSR_IA32_SYSENTER_EIP:     return 0x00000176;
    case MSR_IA32_MISC_ENABLE:      return 0x000001a0;
    case MSR_HYPERVISOR:            return 0x40000000;
    default:
        *err = 1;
        return 0;
    }
}

void *
kvm_get_memory_kvmi(vmi_instance_t vmi, addr_t paddr, uint32_t length) {
    kvm_instance_t *kvm = kvm_get_instance(vmi);
    void *buffer;

    if (!kvm->kvmi_dom)
        return NULL;

    buffer = g_malloc0(length);
    if (kvmi_read_physical(kvm->kvmi_dom, paddr, buffer, length) < 0) {
        g_free(buffer);
        return NULL;
    }

    return buffer;
}


void
kvm_release_memory(
    void *memory,
    size_t UNUSED(length))
{
    if (memory)
        free(memory);
}

/**
 * Setup KVM live (i.e. KVM patch or KVM native) mode.
 * If KVM patch has been setup before, resume it.
 * If KVM patch hasn't been setup but is available, setup
 * KVM patch, otherwise setup KVM native.
 */
status_t
kvm_setup_live_mode(
    vmi_instance_t vmi)
{
    memory_cache_destroy(vmi);
    memory_cache_init(vmi, kvm_get_memory_kvmi, kvm_release_memory, 1);
    return VMI_SUCCESS;
}

//----------------------------------------------------------------------------
// KVMI-Specific Interface Functions (no direction mapping to driver_*)

static int
cb_kvmi_connect(
    void *dom,
    unsigned char (*uuid)[16],
    void *ctx)
{
    kvm_instance_t *kvm = ctx; 

    pthread_mutex_lock(&kvm->kvm_connect_mutex);
    /*
     * If kvmi_dom is not NULL it means this is a reconnection.
     * The previous connection was closed somehow.
     */
    kvmi_domain_close(kvm->kvmi_dom);
    kvm->kvmi_dom = dom;
    pthread_cond_signal(&kvm->kvm_start_cond);
    pthread_mutex_unlock(&kvm->kvm_connect_mutex);

    return 0;
}

/*
 * This callback is not used unless some events are enabled
 * with the kvmi_control_events() command.
 */
static int
cb_new_event(
    void *UNUSED(dom),
    unsigned int seq,
    unsigned int size,
    void *UNUSED(ctx))
{
    dbprint(VMI_DEBUG_KVM, "--event seq:%u size:%u\n", seq, size);

    /* return kvmi_reply_event(dom, seq, &rpl, sizeof(rpl)) */

    return 0;
}

static bool
init_kvmi(
    kvm_instance_t *kvm,
    const char *sock_path)
{
    int err = -1;

    pthread_mutex_init(&kvm->kvm_connect_mutex, NULL);
    pthread_cond_init(&kvm->kvm_start_cond, NULL);
    kvm->kvmi_dom = NULL;

    pthread_mutex_lock(&kvm->kvm_connect_mutex);
    kvm->kvmi = kvmi_init_unix_socket(sock_path, cb_kvmi_connect, cb_new_event, kvm);
    if (kvm->kvmi) {
        struct timeval now;
        if (gettimeofday(&now, NULL) == 0) {
           struct timespec t = {};
           t.tv_sec = now.tv_sec + 10;
           err = pthread_cond_timedwait(&kvm->kvm_start_cond, &kvm->kvm_connect_mutex, &t);
        }
    }
    pthread_mutex_unlock(&kvm->kvm_connect_mutex);

    if (err) {
        /*
         * The libkvmi may accept the connection after timeout
         * and our callback can set kvm->kvmi_dom. So, we must
         * stop the accepting thread first.
         */
        kvmi_uninit(kvm->kvmi);
        kvm->kvmi = NULL;
        /* From this point, kvm->kvmi_dom won't be touched. */
        kvmi_domain_close(kvm->kvmi_dom);
        return false;
    }

    return true;
}

static bool
get_kvmi_registers(
    kvm_instance_t *kvm,
    reg_t reg,
    uint64_t *value)
{
    struct kvm_regs regs;
    struct kvm_sregs sregs;
    struct {
        struct kvm_msrs      msrs;
        struct kvm_msr_entry entries[2];
    } msrs = {0};
    unsigned int mode;
    unsigned short vcpu = 0;
    int err;

    if (!kvm->kvmi_dom)
        return false;

    msrs.msrs.nmsrs = sizeof(msrs.entries)/sizeof(msrs.entries[0]);
    msrs.entries[0].index = translate_msr_index(MSR_EFER, &err);
    msrs.entries[1].index = translate_msr_index(MSR_STAR, &err);

    err = kvmi_get_registers(kvm->kvmi_dom, vcpu, &regs, &sregs, &msrs.msrs, &mode);

    if (err != 0)
        return false;

    /* mode should be 8 if VMI_PM_IA32E == vmi->page_mode */

    switch (reg) {
    case RAX:
        *value = regs.rax;
        break;
    case RBX:
        *value = regs.rbx;
        break;
    case RCX:
        *value = regs.rcx;
        break;
    case RDX:
        *value = regs.rdx;
        break;
    case RBP:
        *value = regs.rbp;
        break;
    case RSI:
        *value = regs.rsi;
        break;
    case RDI:
        *value = regs.rdi;
        break;
    case RSP:
        *value = regs.rsp;
        break;
    case R8:
        *value = regs.r8;
        break;
    case R9:
        *value = regs.r9;
        break;
    case R10:
        *value = regs.r10;
        break;
    case R11:
        *value = regs.r11;
        break;
    case R12:
        *value = regs.r12;
        break;
    case R13:
        *value = regs.r13;
        break;
    case R14:
        *value = regs.r14;
        break;
    case R15:
        *value = regs.r15;
        break;
    case RIP:
        *value = regs.rip;
        break;
    case RFLAGS:
        *value = regs.rflags;
        break;
    case CR0:
        *value = sregs.cr0;
        break;
    case CR2:
        *value = sregs.cr2;
        break;
    case CR3:
        *value = sregs.cr3;
        break;
    case CR4:
        *value = sregs.cr4;
        break;
    case MSR_EFER:
        *value = msrs.entries[0].data;
        break;
    case MSR_STAR:
        *value = msrs.entries[1].data;
        break;
    default:
        return false;
    }

    return true;
}

//----------------------------------------------------------------------------
// General Interface Functions (1-1 mapping to driver_* function)

status_t
kvm_init(
    vmi_instance_t vmi,
    uint32_t UNUSED(init_flags),
    void *UNUSED(init_data))
{
    kvm_instance_t *kvm = g_malloc0(sizeof(kvm_instance_t));
    if ( VMI_FAILURE == create_libvirt_wrapper(kvm) )
        return VMI_FAILURE;

    virConnectPtr conn = kvm->libvirt.virConnectOpenAuth("qemu:///system", kvm->libvirt.virConnectAuthPtrDefault, 0);
    if (NULL == conn) {
        dbprint(VMI_DEBUG_KVM, "--no connection to kvm hypervisor\n");
        free(kvm);
        return VMI_FAILURE;
    }

    kvm->conn = conn;

    vmi->driver.driver_data = (void*)kvm;

    return VMI_SUCCESS;
}

status_t
kvm_init_vmi(
    vmi_instance_t vmi,
    uint32_t UNUSED(init_flags),
    void *init_data)
{
    kvm_instance_t *kvm = kvm_get_instance(vmi);
    virDomainInfo info;
    virDomainPtr dom = kvm->libvirt.virDomainLookupByID(kvm->conn, kvm->id);
    if (NULL == dom) {
        dbprint(VMI_DEBUG_KVM, "--failed to find kvm domain\n");
        return VMI_FAILURE;
    }

    // get the libvirt version
    unsigned long libVer = 0;

    if (kvm->libvirt.virConnectGetLibVersion(kvm->conn, &libVer) != 0) {
        dbprint(VMI_DEBUG_KVM, "--failed to get libvirt version\n");
        return VMI_FAILURE;
    }
    dbprint(VMI_DEBUG_KVM, "--libvirt version %lu\n", libVer);

    kvm->dom = dom;
    vmi->vm_type = NORMAL;

    //get the VCPU count from virDomainInfo structure
    if (-1 == kvm->libvirt.virDomainGetInfo(kvm->dom, &info)) {
        dbprint(VMI_DEBUG_KVM, "--failed to get vm info\n");
        return VMI_FAILURE;
    }
    vmi->num_vcpus = info.nrVirtCpu;

    dbprint(VMI_DEBUG_KVM, "--Connecting to KVMI...\n");
    if (!init_kvmi(kvm, (char *)init_data)) {
        dbprint(VMI_DEBUG_KVM, "--KVMI failed\n");
        return VMI_FAILURE;
    }
    dbprint(VMI_DEBUG_KVM, "--KVMI connected\n");

    return kvm_setup_live_mode(vmi);
}

void
kvm_destroy(
    vmi_instance_t vmi)
{
    kvm_instance_t *kvm = kvm_get_instance(vmi);

    kvmi_uninit(kvm->kvmi); /* closes the accepting thread */
    kvm->kvmi = NULL;
    kvmi_domain_close(kvm->kvmi_dom);
    kvm->kvmi_dom = NULL;

    if (kvm->dom) {
        kvm->libvirt.virDomainFree(kvm->dom);
    }

    if (kvm->conn) {
        kvm->libvirt.virConnectClose(kvm->conn);
    }

    dlclose(kvm->libvirt.handle);
}

uint64_t
kvm_get_id_from_name(
    vmi_instance_t vmi,
    const char *name)
{
    virDomainPtr dom = NULL;
    uint64_t domainid = VMI_INVALID_DOMID;
    kvm_instance_t *kvm = kvm_get_instance(vmi);

    dom = kvm->libvirt.virDomainLookupByName(kvm->conn, name);
    if (NULL == dom) {
        dbprint(VMI_DEBUG_KVM, "--failed to find kvm domain\n");
        domainid = VMI_INVALID_DOMID;
    } else {

        domainid = (uint64_t) kvm->libvirt.virDomainGetID(dom);
        if (domainid == (uint64_t)-1) {
            dbprint(VMI_DEBUG_KVM, "--requested kvm domain may not be running\n");
            domainid = VMI_INVALID_DOMID;
        }
    }

    if (dom)
        kvm->libvirt.virDomainFree(dom);

    return domainid;
}

status_t
kvm_get_name_from_id(
    vmi_instance_t vmi,
    uint64_t domainid,
    char **name)
{
    virDomainPtr dom = NULL;
    const char* temp_name = NULL;
    kvm_instance_t *kvm = kvm_get_instance(vmi);

    dom = kvm->libvirt.virDomainLookupByID(kvm->conn, domainid);
    if (NULL == dom) {
        dbprint(VMI_DEBUG_KVM, "--failed to find kvm domain\n");
        return VMI_FAILURE;
    }

    temp_name = kvm->libvirt.virDomainGetName(dom);
    *name = temp_name ? strdup(temp_name) : NULL;

    if (dom)
        kvm->libvirt.virDomainFree(dom);

    if (*name) {
        return VMI_SUCCESS;
    }

    return VMI_FAILURE;
}

uint64_t
kvm_get_id(
    vmi_instance_t vmi)
{
    return kvm_get_instance(vmi)->id;
}

void
kvm_set_id(
    vmi_instance_t vmi,
    uint64_t domainid)
{
    kvm_get_instance(vmi)->id = domainid;
}

status_t
kvm_check_id(
    vmi_instance_t vmi,
    uint64_t domainid)
{
    virDomainPtr dom = NULL;
    kvm_instance_t *kvm = kvm_get_instance(vmi);

    dom = kvm->libvirt.virDomainLookupByID(kvm->conn, domainid);
    if (NULL == dom) {
        dbprint(VMI_DEBUG_KVM, "--failed to find kvm domain\n");
        return VMI_FAILURE;
    }

    if (dom)
        kvm->libvirt.virDomainFree(dom);

    return VMI_SUCCESS;
}

status_t
kvm_get_name(
    vmi_instance_t vmi,
    char **name)
{
    kvm_instance_t *kvm = kvm_get_instance(vmi);

    const char *tmpname = kvm->libvirt.virDomainGetName(kvm->dom);

    // don't need to deallocate the name, it will go away with the domain object

    if (NULL != tmpname) {
        *name = strdup(tmpname);
        return VMI_SUCCESS;
    } else {
        return VMI_FAILURE;
    }
}

void
kvm_set_name(
    vmi_instance_t vmi,
    const char *name)
{
    kvm_get_instance(vmi)->name = strndup(name, 500);
}

status_t
kvm_get_memsize(
    vmi_instance_t vmi,
    uint64_t *allocated_ram_size,
    addr_t *maximum_physical_address)
{
    kvm_instance_t *kvm = kvm_get_instance(vmi);
    virDomainInfo info;

    if (-1 == kvm->libvirt.virDomainGetInfo(kvm->dom, &info)) {
        dbprint(VMI_DEBUG_KVM, "--failed to get vm info\n");
        goto error_exit;
    }
    *allocated_ram_size = info.maxMem * 1024; // convert KBytes to bytes
    *maximum_physical_address = *allocated_ram_size;

    return VMI_SUCCESS;
error_exit:
    return VMI_FAILURE;
}

status_t
kvm_get_vcpuregs(
    vmi_instance_t vmi,
    registers_t *registers,
    unsigned long vcpu)
{
    struct kvm_regs regs;
    struct kvm_sregs sregs;
    struct {
        struct kvm_msrs msrs;
        struct kvm_msr_entry entries[6];
    } msrs = { 0 };
    int err;
    unsigned int mode;
    x86_registers_t *x86 = &registers->x86;
    kvm_instance_t *kvm = kvm_get_instance(vmi);

    msrs.msrs.nmsrs = sizeof(msrs.entries)/sizeof(msrs.entries[0]);
    msrs.entries[0].index = translate_msr_index(MSR_IA32_SYSENTER_CS, &err);
    msrs.entries[1].index = translate_msr_index(MSR_IA32_SYSENTER_ESP, &err);
    msrs.entries[2].index = translate_msr_index(MSR_IA32_SYSENTER_EIP, &err);
    msrs.entries[3].index = translate_msr_index(MSR_EFER, &err);
    msrs.entries[4].index = translate_msr_index(MSR_STAR, &err);
    msrs.entries[5].index = translate_msr_index(MSR_LSTAR, &err);

    if (!kvm->kvmi_dom)
        return VMI_FAILURE;

    err = kvmi_get_registers(kvm->kvmi_dom, vcpu, &regs, &sregs, &msrs.msrs, &mode);

    if (err != 0)
        return VMI_FAILURE;

    x86->rax = regs.rax;
    x86->rcx = regs.rcx;
    x86->rdx = regs.rdx;
    x86->rbx = regs.rbx;
    x86->rsp = regs.rsp;
    x86->rbp = regs.rbp;
    x86->rsi = regs.rsi;
    x86->rdi = regs.rdi;
    x86->r8 = regs.r8;
    x86->r9 = regs.r9;
    x86->r10 = regs.r10;
    x86->r11 = regs.r11;
    x86->r12 = regs.r12;
    x86->r13 = regs.r13;
    x86->r14 = regs.r14;
    x86->r15 = regs.r15;
    x86->rflags = regs.rflags;
    x86->dr7 = 0; // FIXME: where do I get this
    x86->rip = regs.rip;
    x86->cr0 = sregs.cr0;
    x86->cr2 = sregs.cr2;
    x86->cr3 = sregs.cr3;
    x86->cr4 = sregs.cr4;
    // Are these correct
    x86->sysenter_cs = msrs.entries[0].data;
    x86->sysenter_esp = msrs.entries[1].data;
    x86->sysenter_eip = msrs.entries[2].data;
    x86->msr_efer = msrs.entries[3].data;
    x86->msr_star = msrs.entries[4].data;
    x86->msr_lstar = msrs.entries[5].data;
    x86->fs_base = 0; // FIXME: Where do I get these
    x86->gs_base = 0;
    x86->cs_arbytes = 0;
    x86->_pad = 0;

    return VMI_SUCCESS;
}

status_t
kvm_set_vcpureg(vmi_instance_t vmi,
                uint64_t value,
                reg_t reg,
                unsigned long vcpu) {
    kvm_instance_t *kvm = kvm_get_instance(vmi);
    if (!kvm->kvmi_dom)
        return VMI_FAILURE;
    unsigned int mode = 0;
    struct kvm_regs regs = {0};
    struct kvm_sregs sregs = {0};
    struct {
        struct kvm_msrs msrs;
        struct kvm_msr_entry entries[0];
    } msrs = {0};
    msrs.msrs.nmsrs = 0;

    if (kvmi_get_registers(kvm->kvmi_dom, vcpu, &regs, &sregs, &msrs.msrs, &mode) < 0) {
        return VMI_FAILURE;
    }

    // This could use a macro or something
    switch (reg) {
    case RAX:
        regs.rax = value;
        break;
    case RBX:
        regs.rbx = value;
        break;
    case RCX:
        regs.rcx = value;
        break;
    case RDX:
        regs.rdx = value;
        break;
    case RSI:
        regs.rsi = value;
        break;
    case RDI:
        regs.rdi = value;
        break;
    case RSP:
        regs.rsp = value;
        break;
    case RBP:
        regs.rbp = value;
        break;
    case R8:
        regs.r8 = value;
        break;
    case R9:
        regs.r9 = value;
        break;
    case R10:
        regs.r10 = value;
        break;
    case R11:
        regs.r11 = value;
        break;
    case R12:
        regs.r12 = value;
        break;
    case R13:
        regs.r13 = value;
        break;
    case R14:
        regs.r14 = value;
        break;
    case R15:
        regs.r15 = value;
        break;
    case RIP:
        regs.rip = value;
        break;
    case RFLAGS:
        regs.rflags = value;
        break;
    default:
        return VMI_FAILURE;
    }

    if (kvmi_set_registers(kvm->kvmi_dom, vcpu, &regs) < 0) {
        return VMI_FAILURE;
    }

    return VMI_SUCCESS;
}

status_t kvm_set_vcpuregs(vmi_instance_t vmi,
                          registers_t *registers,
                          unsigned long vcpu) {
    kvm_instance_t *kvm = kvm_get_instance(vmi);
    if (!kvm->kvmi_dom)
        return VMI_FAILURE;
    struct x86_regs *x86 = &registers->x86;
    struct kvm_regs regs = {
        .rax = x86->rax,
        .rbx = x86->rbx,
        .rcx = x86->rcx,
        .rdx = x86->rdx,
        .rsi = x86->rsi,
        .rdi = x86->rdi,
        .rsp = x86->rsp,
        .rbp = x86->rbp,
        .r8  = x86->r8,
        .r9  = x86->r9,
        .r10 = x86->r10,
        .r11 = x86->r11,
        .r12 = x86->r12,
        .r13 = x86->r13,
        .r14 = x86->r14,
        .r15 = x86->r15,
        .rip = x86->rip,
        .rflags = x86->rflags
    };
    if (kvmi_set_registers(kvm->kvmi_dom, vcpu, &regs) < 0) {
        return VMI_FAILURE;
    }
    return VMI_SUCCESS;
}

status_t
kvm_get_vcpureg(
    vmi_instance_t vmi,
    uint64_t *value,
    reg_t reg,
    unsigned long UNUSED(vcpu))
{
    if (get_kvmi_registers(kvm_get_instance(vmi), reg, value))
        return VMI_SUCCESS;

    return VMI_FAILURE;
}

void *
kvm_read_page(
    vmi_instance_t vmi,
    addr_t page)
{
    addr_t paddr = page << vmi->page_shift;

    return memory_cache_insert(vmi, paddr);
}

int
kvm_is_pv(
    vmi_instance_t UNUSED(vmi))
{
    return 0;
}

status_t
kvm_test(
    uint64_t domainid,
    const char *name)
{
    struct vmi_instance _vmi = {0};
    vmi_instance_t vmi = &_vmi;

    if ( VMI_FAILURE == kvm_init(vmi, 0, NULL) )
        return VMI_FAILURE;

    if (name) {
        domainid = kvm_get_id_from_name(vmi, name);
        if (domainid != VMI_INVALID_DOMID)
            return VMI_SUCCESS;
    }

    if (domainid != VMI_INVALID_DOMID) {
        char *_name = NULL;
        status_t rc = kvm_get_name_from_id(vmi, domainid, &_name);
        free(_name);

        if ( VMI_SUCCESS == rc )
            return rc;
    }

    kvm_destroy(vmi);
    return VMI_FAILURE;
}

status_t
kvm_pause_vm(
    vmi_instance_t vmi)
{
    kvm_instance_t *kvm = kvm_get_instance(vmi);

    if (-1 == kvm->libvirt.virDomainSuspend(kvm->dom)) {
        return VMI_FAILURE;
    }
    return VMI_SUCCESS;
}

status_t
kvm_resume_vm(
    vmi_instance_t vmi)
{
    kvm_instance_t *kvm = kvm_get_instance(vmi);

    if (-1 == kvm->libvirt.virDomainResume(kvm->dom)) {
        return VMI_FAILURE;
    }
    return VMI_SUCCESS;
}
