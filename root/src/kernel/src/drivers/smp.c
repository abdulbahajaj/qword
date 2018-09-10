#include <stdint.h>
#include <stddef.h>
#include <klib.h>
#include <apic.h>
#include <acpi.h>
#include <acpi/madt.h>
#include <panic.h>
#include <smp.h>
#include <time.h>
#include <mm.h>
#include <task.h>

#define CPU_STACK_SIZE 16384

size_t smp_cpu_count = 1;

typedef struct {
    uint32_t unused __attribute__((aligned(16)));
    uint64_t sp;
    uint32_t entries[23];
} __attribute__((packed)) tss_t;

static size_t cpu_stack_top = KERNEL_PHYS_OFFSET + 0xeffff0;

cpu_local_t cpu_locals[MAX_CPUS];
static tss_t cpu_tss[MAX_CPUS] __attribute__((aligned(16)));

static void ap_kernel_entry(void) {
    /* APs jump here after initialisation */

    kprint(KPRN_INFO, "smp: Started up AP #%u", current_cpu);
    kprint(KPRN_INFO, "smp: Kernel stack top: %X", cpu_locals[current_cpu].kernel_stack);
    
    /* Enable this AP's local APIC */
    lapic_enable();
    
    for (;;) { asm volatile ("sti; hlt"); }
    return;
}

static inline void setup_cpu_local(int cpu_number, uint8_t lapic_id) {
    /* Prepare CPU local */
    cpu_locals[cpu_number].cpu_number = cpu_number;
    cpu_locals[cpu_number].kernel_stack = cpu_stack_top;
    cpu_locals[cpu_number].current_process = -1;
    cpu_locals[cpu_number].current_thread = -1;
    cpu_locals[cpu_number].lapic_id = lapic_id;
    cpu_locals[cpu_number].reset_scheduler = 0;

    /* Prepare TSS */
    cpu_tss[cpu_number].sp = (uint64_t)cpu_stack_top;

    return;
}

static int start_ap(uint8_t target_apic_id, int cpu_number) {
    if (cpu_number == MAX_CPUS) {
        panic("smp: CPU limit exceeded", smp_cpu_count, 0);
    }

    setup_cpu_local(cpu_number, target_apic_id);

    cpu_local_t *cpu_local = &cpu_locals[cpu_number];
    tss_t *tss = &cpu_tss[cpu_number];

    void *trampoline = smp_prepare_trampoline(ap_kernel_entry, (void *)kernel_pagemap.pagemap,
                                (void *)cpu_stack_top, cpu_local, tss);

    /* Send the INIT IPI */
    lapic_write(APICREG_ICR1, ((uint32_t)target_apic_id) << 24);
    lapic_write(APICREG_ICR0, 0x4500);
    /* wait 10ms */
    ksleep(10);
    /* Send the Startup IPI */
    lapic_write(APICREG_ICR1, ((uint32_t)target_apic_id) << 24);
    lapic_write(APICREG_ICR0, 0x4600 | (uint32_t)(size_t)trampoline);
    /* wait 1ms */
    ksleep(1);

    if (smp_check_ap_flag()) {
        goto success;
    } else {
        /* Send the Startup IPI again */
        lapic_write(APICREG_ICR1, ((uint32_t)target_apic_id) << 24);
        lapic_write(APICREG_ICR0, 0x4600 | (uint32_t)(size_t)trampoline);
        /* wait 1s */
        ksleep(1000);
        if (smp_check_ap_flag())
            goto success;
        else
            return -1;
    }

success:
    cpu_stack_top -= CPU_STACK_SIZE;
    return 0;
}

static void init_cpu0(void) {
    setup_cpu_local(0, 0);

    cpu_local_t *cpu_local = &cpu_locals[0];
    tss_t *tss = &cpu_tss[0];

    smp_init_cpu0_local(cpu_local, tss);

    cpu_stack_top -= CPU_STACK_SIZE;

    return;
}

void init_smp(void) {
    /* prepare CPU 0 first */
    init_cpu0();

    /* start up the APs and jump them into the kernel */
    for (size_t i = 1; i < madt_local_apic_i; i++) {
        kprint(KPRN_INFO, "smp: Starting up AP #%u", i);
        if (start_ap(madt_local_apics[i]->apic_id, smp_cpu_count)) {
            kprint(KPRN_ERR, "smp: Failed to start AP #%u", i);
            continue;
        }
        smp_cpu_count++;
        /* wait a bit */
        ksleep(10);
    }

    kprint(KPRN_INFO, "smp: Total CPU count: %u", smp_cpu_count);

    return;
}