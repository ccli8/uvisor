/*
 * Copyright (c) 2016, ARM Limited, All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <uvisor.h>
#include "context.h"
#include "debug.h"
#include "exc_return.h"
#include "page_allocator_faults.h"
#include "vmpu.h"
#include "vmpu_mpu.h"
#include <stdbool.h>

static const MpuRegion* vmpu_fault_find_region(uint32_t fault_addr)
{
    const MpuRegion *region;

    /* Check current box if not base. */
    if ((g_active_box) && ((region = vmpu_region_find_for_address(g_active_box, fault_addr)) != NULL)) {
        return region;
    }

    /* Check base-box. */
    if ((region = vmpu_region_find_for_address(0, fault_addr)) != NULL) {
        return region;
    }

    /* If no region was found */
    return NULL;
}

uint32_t vmpu_fault_find_acl(uint32_t fault_addr, uint32_t size)
{
    const MpuRegion *region;

    /* Return ACL if available. */
    /* FIXME: Use SECURE_ACCESS for SCR! */
    if (fault_addr == (uint32_t) &SCB->SCR) {
        return UVISOR_TACL_UWRITE | UVISOR_TACL_UREAD;
    }

    /* Translate fault_addr into its physical address if it is in the bit-banding region. */
    if (fault_addr >= VMPU_PERIPH_BITBAND_START && fault_addr <= VMPU_PERIPH_BITBAND_END) {
        fault_addr = VMPU_PERIPH_BITBAND_ALIAS_TO_ADDR(fault_addr);
    } else if (fault_addr >= VMPU_SRAM_BITBAND_START && fault_addr <= VMPU_SRAM_BITBAND_END) {
        fault_addr = VMPU_SRAM_BITBAND_ALIAS_TO_ADDR(fault_addr);
    }

    /* Search base box and active box ACLs. */
    if (!(region = vmpu_fault_find_region(fault_addr))) {
        return 0;
    }

    /* Ensure that data fits in selected region. */
    if ((fault_addr + size) > region->end) {
        return 0;
    }

    return region->acl;
}

static int vmpu_mem_push_page_acl_iterator(uint32_t start_addr, uint32_t end_addr, uint8_t page)
{
    (void) page;
    MpuRegion region = {.start = start_addr, .end = end_addr, .config = 1};
    /* We only continue if we have not wrapped around the end of the MPU regions yet. */
    return vmpu_mpu_push(&region, 100);
}

int vmpu_fault_recovery_mpu(uint32_t pc, uint32_t sp, uint32_t fault_addr, uint32_t fault_status)
{
    const MpuRegion *region;
    uint32_t start_addr, end_addr;
    uint8_t page;

    if (page_allocator_get_active_region_for_address(fault_addr, &start_addr, &end_addr, &page) == UVISOR_ERROR_PAGE_OK) {
        /* Remember this fault. */
        page_allocator_register_fault(page);

        vmpu_mem_push_page_acl_iterator(start_addr, end_addr, g_active_box);
    } else {
        /* Find region for faulting address. */
        if ((region = vmpu_fault_find_region(fault_addr)) == NULL) {
            return 0;
        }

        vmpu_mpu_push(region, 3);
    }

    return 1;
}

uint32_t vmpu_sys_mux_handler(uint32_t lr, uint32_t msp_s)
{
    uint32_t pc;
    uint32_t fault_addr, fault_status;
    int recovered = 0;

    /* The IPSR enumerates interrupt numbers from 0 up, while *_IRQn numbers are
     * both positive (hardware IRQn) and negative (system IRQn). Here we convert
     * the IPSR value to this latter encoding. */
    int ipsr = ((int) (__get_IPSR() & 0x1FF)) - NVIC_OFFSET;

    /* Determine the exception origin. */
    bool from_s = EXC_FROM_S(lr);
    bool from_np = EXC_FROM_NP(lr);
    bool from_psp = EXC_FROM_PSP(lr);
    uint32_t sp = from_s ? (from_np ? (from_psp ? __get_PSP() : msp_s) : msp_s) :
                           (from_np ? (from_psp ? __TZ_get_PSP_NS() : __TZ_get_MSP_NS()) : __TZ_get_MSP_NS());

    switch(ipsr) {
        case NonMaskableInt_IRQn:
            HALT_ERROR(NOT_IMPLEMENTED, "No NonMaskableInt IRQ handler registered.");
            break;

        case HardFault_IRQn:
            DEBUG_FAULT(FAULT_HARD, lr, sp);
            HALT_ERROR(FAULT_HARD, "Cannot recover from a hard fault.");
            break;

        case MemoryManagement_IRQn:
            DEBUG_FAULT(FAULT_MEMMANAGE, lr, sp);
            HALT_ERROR(FAULT_MEMMANAGE, "Cannot recover from a memory management fault.");
            break;

        case BusFault_IRQn:
            DEBUG_FAULT(FAULT_BUS, lr, sp);
            HALT_ERROR(FAULT_BUS, "Cannot recover from a bus fault.");
            break;

        case UsageFault_IRQn:
            DEBUG_FAULT(FAULT_USAGE, lr, sp);
            HALT_ERROR(FAULT_USAGE, "Cannot recover from a usage fault.");
            break;

        case SecureFault_IRQn:
            fault_status = SAU->SFSR;
            if ((fault_status & (SAU_SFSR_AUVIOL_Msk | SAU_SFSR_SFARVALID_Msk)) ==
                                (SAU_SFSR_AUVIOL_Msk | SAU_SFSR_SFARVALID_Msk)) {
                pc = vmpu_unpriv_uint32_read(sp + (6 * 4));
                fault_addr = SAU->SFAR;
                recovered = vmpu_fault_recovery_mpu(pc, sp, fault_addr, fault_status);
                if (recovered) {
                    SAU->SFSR = fault_status;
                    return lr;
                }
            }
            DEBUG_FAULT(FAULT_SECURE, lr, sp);
            HALT_ERROR(PERMISSION_DENIED, "Cannot recover from a secure fault.");
            break;

        case SVCall_IRQn:
            HALT_ERROR(NOT_IMPLEMENTED, "No SVCall IRQ handler registered.");
            break;

        case DebugMonitor_IRQn:
            DEBUG_FAULT(FAULT_DEBUG, lr, sp);
            HALT_ERROR(FAULT_DEBUG, "Cannot recover from a DebugMonitor fault.");
            break;

        case PendSV_IRQn:
            HALT_ERROR(NOT_IMPLEMENTED, "No PendSV IRQ handler registered.");
            break;

        case SysTick_IRQn:
            HALT_ERROR(NOT_IMPLEMENTED, "No SysTick IRQ handler registered.");
            break;

        default:
            HALT_ERROR(NOT_ALLOWED, "Active IRQn (%i) is not a system interrupt.", ipsr);
            break;
    }

    return lr;
}

/* This function assumes that its inputs are validated. */
/* FIXME: We've added very simple MPU region switching. - Optimize! */
void vmpu_switch(uint8_t src_box, uint8_t dst_box)
{
    uint32_t dst_count = 0;
    const MpuRegion * region;

    /* DPRINTF("switching from %i to %i\n\r", src_box, dst_box); */

    vmpu_mpu_invalidate();

    /* Only write stack and context ACL for secure boxes. */
    if (dst_box) {
        /* Update target box first to make target stack available. */
        vmpu_region_get_for_box(dst_box, &region, &dst_count);
        /* Push the stack and context protection ACL into ARMv8M_SAU_REGIONS_STATIC. */
        vmpu_mpu_push(region, 255);
        region++;
        dst_count--;
    }

    /* Push one ACL for the page heap into place. */
    page_allocator_iterate_active_pages(vmpu_mem_push_page_acl_iterator, PAGE_ALLOCATOR_ITERATOR_DIRECTION_FORWARD);
    /* g_mpu_slot may now have been incremented by one, if page heap is used by this box. */

    while (dst_count-- && vmpu_mpu_push(region++, 2));

    if (!dst_box) {
        /* Handle public box ACLs last. */
        vmpu_region_get_for_box(0, &region, &dst_count);

        while (dst_count-- && vmpu_mpu_push(region++, 1));
    }
}

void vmpu_acl_sram(uint8_t box_id, uint32_t bss_size, uint32_t stack_size, uint32_t * bss_start,
                   uint32_t * stack_pointer)
{
    static uint32_t g_box_mem_pos = 0;

    if (!g_box_mem_pos) {
        /* Initialize box memories. Leave stack-band sized gap. */
        g_box_mem_pos = UVISOR_REGION_ROUND_UP(
            (uint32_t)__uvisor_config.bss_boxes_start) +
            UVISOR_STACK_BAND_SIZE;
    }

    /* Ensure stack & context alignment. */
    stack_size = UVISOR_REGION_ROUND_UP(UVISOR_MIN_STACK(stack_size));

    /* Add stack ACL. */
    vmpu_region_add_static_acl(
        box_id,
        g_box_mem_pos,
        stack_size,
        UVISOR_TACLDEF_STACK,
        0
    );

    /* Set stack pointer to box stack size minus guard band. */
    g_box_mem_pos += stack_size;
    *stack_pointer = g_box_mem_pos;
    /* Add stack protection band. */
    g_box_mem_pos += UVISOR_STACK_BAND_SIZE;

    /* Add context ACL. */
    assert(bss_size != 0);
    bss_size = UVISOR_REGION_ROUND_UP(bss_size);
    *bss_start = g_box_mem_pos;

    /* Add context ACL. */
    vmpu_region_add_static_acl(
        box_id,
        g_box_mem_pos,
        bss_size,
        UVISOR_TACLDEF_DATA,
        0
    );

    g_box_mem_pos += bss_size + UVISOR_STACK_BAND_SIZE;
}

void vmpu_arch_init(void)
{
    /* AIRCR needs to be unlocked with this key on every write. */
    const uint32_t SCB_AIRCR_VECTKEY = 0x5FAUL;

    /* AIRCR configurations:
     *      - Non-secure exceptions are de-prioritized.
     *      - BusFault, HardFault, and NMI are Secure.
     */
    /* TODO: Setup a sensible priority grouping. */
    uint32_t aircr = SCB->AIRCR;
    SCB->AIRCR = (SCB_AIRCR_VECTKEY << SCB_AIRCR_VECTKEY_Pos) |
                 (aircr & SCB_AIRCR_ENDIANESS_Msk) |   /* Keep unchanged */
                 (SCB_AIRCR_PRIS_Msk) |
                 (0 << SCB_AIRCR_BFHFNMINS_Pos) |
                 (aircr & SCB_AIRCR_PRIGROUP_Msk) |    /* Keep unchanged */
                 (0 << SCB_AIRCR_SYSRESETREQ_Pos) |
                 (0 << SCB_AIRCR_VECTCLRACTIVE_Pos);

    /* SHCSR configurations:
     *      - SecureFault exception enabled.
     *      - UsageFault exception enabled for the selected Security state.
     *      - BusFault exception enabled.
     *      - MemManage exception enabled for the selected Security state.
     */
    SCB->SHCSR |= (SCB_SHCSR_SECUREFAULTENA_Msk) |
                  (SCB_SHCSR_USGFAULTENA_Msk) |
                  (SCB_SHCSR_BUSFAULTENA_Msk) |
                  (SCB_SHCSR_MEMFAULTENA_Msk);

    vmpu_mpu_init();

    /* Public uVisor flash */
    vmpu_mpu_set_static_acl(0, (uint32_t) __uvisor_config.flash_start,
        ((uint32_t) &__uvisor_entry_points_start__) - ((uint32_t) __uvisor_config.flash_start),
        UVISOR_TACL_UEXECUTE | UVISOR_TACL_UREAD | UVISOR_TACL_UWRITE,
        0
    );
    /* Entry points */
    vmpu_mpu_set_static_acl(1, (uint32_t) &__uvisor_entry_points_start__,
        ((uint32_t) &__uvisor_entry_points_end__) - ((uint32_t) &__uvisor_entry_points_start__),
        UVISOR_TACL_SEXECUTE | UVISOR_TACL_UEXECUTE,
        SAU_RLAR_NSC_Msk /* Entry point is non-secure callable */
    );
    /* Public flash */
    vmpu_mpu_set_static_acl(2, (uint32_t) &__uvisor_entry_points_end__,
        (uint32_t) __uvisor_config.flash_end - (uint32_t) &__uvisor_entry_points_end__,
        UVISOR_TACL_UEXECUTE | UVISOR_TACL_UREAD | UVISOR_TACL_UWRITE,
        0
    );
    /* Public SRAM */
    vmpu_mpu_set_static_acl(3, (uint32_t) __uvisor_config.page_end,
        (uint32_t) __uvisor_config.sram_end - (uint32_t) __uvisor_config.page_end,
        UVISOR_TACL_UEXECUTE | UVISOR_TACL_UREAD | UVISOR_TACL_UWRITE,
        0
    );

    vmpu_mpu_lock();
}

void vmpu_order_boxes(int * const best_order, int box_count)
{
    for (int i = 0; i < box_count; ++i) {
        best_order[i] = i;
    }
}
