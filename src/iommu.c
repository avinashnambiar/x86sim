/* Intel VT-d remapping unit, interrupt remapping only. One DRHD at
 * DMAR_BASE covers every device (INCLUDE_PCI_ALL) and the I/O APIC; the
 * ACPI DMAR table describes it (bios.c).
 *
 *   - Registers: version, capabilities, global command/status (root and
 *     interrupt-remapping table pointers, translation, queued invalidation,
 *     interrupt remapping and compatibility-format enables), register-based
 *     context/IOTLB invalidation, fault and invalidation-event registers.
 *   - Queued invalidation: descriptors are consumed when the tail is
 *     written; wait descriptors write their status and set ICS.IWC.
 *     Entries are read from memory on every interrupt, so cache
 *     invalidations have nothing to do.
 *   - Interrupt remapping: a remappable-format I/O APIC entry indexes the
 *     table at IRTA; the IRTE gives destination (32-bit in extended mode,
 *     which x2APIC needs), destination mode, delivery mode and vector.
 *     Compatibility-format entries are blocked unless CFI is enabled.
 *
 * DMA translation is accepted (TE, root table) but not applied: none of the
 * PC's devices does bus-master DMA. Source-id validation and fault
 * recording are not modeled: a blocked interrupt is dropped. */
#include "pc_internal.h"
#include "machine.h"
#include <string.h>

enum {
    GCMD_TE = 1u << 31, GCMD_SRTP = 1u << 30, GCMD_WBF = 1u << 27, GCMD_QIE = 1u << 26,
    GCMD_IRE = 1u << 25, GCMD_SIRTP = 1u << 24, GCMD_CFI = 1u << 23,
};
#define IRO 0x50                                  /* IOTLB registers at 0x500 */
#define FRO 0x40                                  /* fault recording at 0x400 */
#define CAP (2ULL                                 /* ND: 256 domains */ \
           | 0x6ULL << 8                          /* SAGAW: 39- and 48-bit tables */ \
           | 47ULL << 16                          /* MGAW: 48 bits */ \
           | (uint64_t)FRO << 24)
#define ECAP ((1ULL << 1)                         /* QI: queued invalidation */ \
            | (1ULL << 3)                         /* IR: interrupt remapping */ \
            | (1ULL << 4)                         /* EIM: 32-bit destinations (x2APIC) */ \
            | (1ULL << 6)                         /* PT: pass-through */ \
            | (uint64_t)IRO << 8)

void iommu_reset(PC *pc) { memset(&pc->iommu, 0, sizeof pc->iommu); }
int iommu_ir_enabled(PC *pc) { return (pc->iommu.gsts & GCMD_IRE) != 0; }

static void dma_read(PC *pc, uint64_t pa, void *buf, size_t n) {
    memset(buf, 0, n);
    machine_phys_peek(pc->m, pa, buf, n);
}

/* Consume the invalidation queue up to the tail. */
static void qi_run(PC *pc) {
    Iommu *u = &pc->iommu;
    if (!(u->gsts & GCMD_QIE)) return;
    uint64_t base = u->iqa & ~0xFFFULL, size = 256ULL << (u->iqa & 7);
    uint64_t head = (u->iqh >> 4) % size, tail = (u->iqt >> 4) % size;
    while (head != tail) {
        uint64_t d[2];
        dma_read(pc, base + head * 16, d, 16);
        if ((d[0] & 0xF) == 5) { /* invalidation wait */
            if (d[0] & (1u << 5)) { /* status write */
                uint32_t data = (uint32_t)(d[0] >> 32);
                machine_phys_poke(pc->m, d[1] & ~3ULL, &data, 4);
            }
            if (d[0] & (1u << 4)) u->ics |= 1; /* IWC */
        }
        /* context, IOTLB, device-TLB, interrupt-entry and PASID invalidations:
         * nothing is cached */
        head = (head + 1) % size;
    }
    u->iqh = head << 4;
}

static void gcmd(PC *pc, uint32_t v) {
    Iommu *u = &pc->iommu;
    uint32_t keep = u->gsts & (GCMD_SRTP | GCMD_SIRTP);
    if ((v & GCMD_QIE) && !(u->gsts & GCMD_QIE)) u->iqh = 0;
    u->gsts = keep | (v & (GCMD_TE | GCMD_QIE | GCMD_IRE | GCMD_CFI));
    if (v & GCMD_SRTP) u->gsts |= GCMD_SRTP;     /* RTPS */
    if (v & GCMD_SIRTP) {                         /* IRTPS: latch the table */
        u->gsts |= GCMD_SIRTP;
        u->irt = u->irta;
    }
}

static uint32_t reg_read(PC *pc, uint32_t off) {
    Iommu *u = &pc->iommu;
    switch (off) {
    case 0x00: return 0x10;                       /* version 1.0 */
    case 0x08: return (uint32_t)CAP;
    case 0x0C: return (uint32_t)(CAP >> 32);
    case 0x10: return (uint32_t)ECAP;
    case 0x14: return (uint32_t)(ECAP >> 32);
    case 0x1C: return u->gsts;
    case 0x20: return (uint32_t)u->rtaddr;
    case 0x24: return (uint32_t)(u->rtaddr >> 32);
    case 0x38: return u->fectl;
    case 0x3C: return u->fedata;
    case 0x40: return u->feaddr;
    case 0x44: return u->feuaddr;
    case 0x64: return u->pmen ? 0x80000001 : 0;   /* EPM and its status */
    case 0x80: return (uint32_t)u->iqh;
    case 0x88: return (uint32_t)u->iqt;
    case 0x90: return (uint32_t)u->iqa;
    case 0x94: return (uint32_t)(u->iqa >> 32);
    case 0x9C: return u->ics;
    case 0xA0: return u->iectl;
    case 0xA4: return u->iedata;
    case 0xA8: return u->ieaddr;
    case 0xAC: return u->ieuaddr;
    case 0xB8: return (uint32_t)u->irta;
    case 0xBC: return (uint32_t)(u->irta >> 32);
    }
    return 0; /* CCMD/IOTLB (invalidation always complete), FSTS, fault records */
}

static void reg_write(PC *pc, uint32_t off, uint32_t v) {
    Iommu *u = &pc->iommu;
    switch (off) {
    case 0x18: gcmd(pc, v); return;
    case 0x20: u->rtaddr = (u->rtaddr & ~0xFFFFFFFFULL) | (v & ~0xFFFu); return;
    case 0x24: u->rtaddr = (u->rtaddr & 0xFFFFFFFFULL) | (uint64_t)v << 32; return;
    case 0x38: u->fectl = v & 0x80000000; return;
    case 0x3C: u->fedata = v; return;
    case 0x40: u->feaddr = v; return;
    case 0x44: u->feuaddr = v; return;
    case 0x64: u->pmen = v >> 31; return;
    case 0x88: u->iqt = v & 0x7FFF0; qi_run(pc); return;
    case 0x90: u->iqa = (u->iqa & ~0xFFFFFFFFULL) | (v & ~0xFF8u); return;
    case 0x94: u->iqa = (u->iqa & 0xFFFFFFFFULL) | (uint64_t)v << 32; return;
    case 0x9C: u->ics &= ~(v & 1); return;        /* write 1 to clear IWC */
    case 0xA0: u->iectl = v & 0x80000000; return;
    case 0xA4: u->iedata = v; return;
    case 0xA8: u->ieaddr = v; return;
    case 0xAC: u->ieuaddr = v; return;
    case 0xB8: u->irta = (u->irta & ~0xFFFFFFFFULL) | (v & ~0x7F0u); return;
    case 0xBC: u->irta = (u->irta & 0xFFFFFFFFULL) | (uint64_t)v << 32; return;
    }
}

int iommu_mmio(PC *pc, uint32_t off, void *buf, int n, int write) {
    uint8_t *b = buf;
    if (write) {
        /* whole 32-bit registers; a 64-bit write is two of them, low first */
        for (int k = 0; k + 4 <= n; k += 4) {
            uint32_t v;
            memcpy(&v, b + k, 4);
            reg_write(pc, (off + (uint32_t)k) & ~3u, v);
        }
        return 1;
    }
    for (int k = 0; k < n; k += 4) {
        uint32_t v = reg_read(pc, (off + (uint32_t)k) & ~3u);
        memcpy(b + k, &v, n - k < 4 ? (size_t)(n - k) : 4);
    }
    return 1;
}

/* Deliver an I/O APIC entry while interrupt remapping is on. */
void iommu_ioapic(PC *pc, uint64_t rte, int level) {
    Iommu *u = &pc->iommu;
    if (!(rte & (1ULL << 48))) { /* compatibility format */
        if (u->gsts & GCMD_CFI)
            apic_message(pc, (uint32_t)(rte >> 56), (rte >> 11) & 1, 0, (int)(rte >> 8) & 7, (int)(rte & 0xFF), level);
        return;
    }
    uint64_t idx = (rte >> 49 & 0x7FFF) | (rte >> 11 & 1) << 15;
    uint64_t entries = 2ULL << (u->irt & 0xF), irte[2];
    if (idx >= entries) return;
    dma_read(pc, (u->irt & ~0xFFFULL) + idx * 16, irte, 16);
    if (!(irte[0] & 1)) return; /* not present */
    int x2 = (u->irt >> 11) & 1;  /* EIME: 32-bit destinations */
    uint32_t dest = x2 ? (uint32_t)(irte[0] >> 32) : (uint32_t)(irte[0] >> 40) & 0xFF;
    apic_message(pc, dest, (irte[0] >> 2) & 1, x2, (int)(irte[0] >> 5) & 7, (int)(irte[0] >> 16) & 0xFF, level);
}
