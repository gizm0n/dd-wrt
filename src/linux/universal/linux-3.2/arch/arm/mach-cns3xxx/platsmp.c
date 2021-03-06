/*
 *  linux/arch/arm/mach-cns3xxx/platsmp.c
 *
 *  Copyright (C) 2002 ARM Ltd.
 *  Copyright 2011 Gateworks Corporation
 *		   Chris Lang <clang@gateworks.com>
 *
 *  All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/smp.h>
#include <linux/io.h>

#include <asm/cacheflush.h>
#include <asm/smp_scu.h>
#include <asm/unified.h>
#include <asm/hardware/gic.h>
#include <mach/platform.h>
#include <mach/cns3xxx.h>

#include <asm/fiq.h>

#include "core.h"

static struct fiq_handler fh = {
	.name = "cns3xxx-fiq"
};

static unsigned int fiq_buffer[8];

#define FIQ_ENABLED         0x80000000
#define FIQ_GENERATE				0x00010000
#define CNS3XXX_MAP_AREA    0x01000000
#define CNS3XXX_UNMAP_AREA  0x02000000
#define CNS3XXX_FLUSH_RANGE 0x03000000

extern void cns3xxx_secondary_startup(void);
extern unsigned char cns3xxx_fiq_start, cns3xxx_fiq_end;
extern unsigned int fiq_number[2];
extern struct cpu_cache_fns cpu_cache;
struct cpu_cache_fns cpu_cache_save;
/*
 * control for which core is the next to come out of the secondary
 * boot "holding pen"
 */
volatile int __cpuinitdata pen_release = -1;

static void __init cns3xxx_set_fiq_regs(void)
{
	struct pt_regs FIQ_regs;
	unsigned int cpu = smp_processor_id();

	if (cpu) {
		FIQ_regs.ARM_ip = (unsigned int)&fiq_buffer[4];
		FIQ_regs.ARM_sp = (unsigned int)MISC_FIQ_CPU(0);
	} else {
		FIQ_regs.ARM_ip = (unsigned int)&fiq_buffer[0];
		FIQ_regs.ARM_sp = (unsigned int)MISC_FIQ_CPU(1);
	}
	set_fiq_regs(&FIQ_regs);
}

static void __init cns3xxx_init_fiq(void)
{
	void *fiqhandler_start;
	unsigned int fiqhandler_length;
	int ret;

	fiqhandler_start = &cns3xxx_fiq_start;
	fiqhandler_length = &cns3xxx_fiq_end - &cns3xxx_fiq_start;

	ret = claim_fiq(&fh);

	if (ret) {
		return;
	}

	set_fiq_handler(fiqhandler_start, fiqhandler_length);
	fiq_buffer[0] = (unsigned int)&fiq_number[0];
	fiq_buffer[3] = 0;
	fiq_buffer[4] = (unsigned int)&fiq_number[1];
	fiq_buffer[7] = 0;
}


/*
 * Write pen_release in a way that is guaranteed to be visible to all
 * observers, irrespective of whether they're taking part in coherency
 * or not.  This is necessary for the hotplug code to work reliably.
 */
static void __cpuinit write_pen_release(int val)
{
	pen_release = val;
	smp_wmb();
	__cpuc_flush_dcache_area((void *)&pen_release, sizeof(pen_release));
	outer_clean_range(__pa(&pen_release), __pa(&pen_release + 1));
}

static void __iomem *scu_base_addr(void)
{
	return (void __iomem *)(CNS3XXX_TC11MP_SCU_BASE_VIRT);
}

static DEFINE_SPINLOCK(boot_lock);

void __cpuinit platform_secondary_init(unsigned int cpu)
{
	/*
	 * if any interrupts are already enabled for the primary
	 * core (e.g. timer irq), then they will not have been enabled
	 * for us: do so
	 */
	gic_secondary_init(0);

	/*
	 * Setup Secondary Core FIQ regs
	 */
	cns3xxx_set_fiq_regs();

	/*
	 * let the primary processor know we're out of the
	 * pen, then head off into the C entry point
	 */
	write_pen_release(-1);
	
	cpu_cache.dma_map_area = (void*)smp_dma_map_area;
	cpu_cache.dma_unmap_area = (void*)smp_dma_unmap_area;
	cpu_cache.dma_flush_range = (void*)smp_dma_flush_range;

	/*
	 * Synchronise with the boot thread.
	 */
	spin_lock(&boot_lock);
	spin_unlock(&boot_lock);
}
extern unsigned int numcpucores;

int __cpuinit boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	unsigned long timeout;

	/*
	 * Set synchronisation state between this boot processor
	 * and the secondary one
	 */
	spin_lock(&boot_lock);

	/*
	 * This is really belt and braces; we hold unintended secondary
	 * CPUs in the holding pen until we're ready for them.  However,
	 * since we haven't sent them a soft interrupt, they shouldn't
	 * be there.
	 */
	write_pen_release(cpu);

	/*
	 * Send the secondary CPU a soft interrupt, thereby causing
	 * the boot monitor to read the system wide flags register,
	 * and branch to the address found there.
	 */
	gic_raise_softirq(cpumask_of(cpu), 1);
//	smp_cross_call(cpumask_of(cpu), 1);

	timeout = jiffies + (1 * HZ);
	while (time_before(jiffies, timeout)) {
		smp_rmb();
		if (pen_release == -1)
		{
			numcpucores++;
			break;
		}

		udelay(10);
	}

	/*
	 * now the secondary core is starting up let it run its
	 * calibrations, then wait for it to finish
	 */
	spin_unlock(&boot_lock);

	return pen_release != -1 ? -ENOSYS : 0;
}

/*
 * Initialise the CPU possible map early - this describes the CPUs
 * which may be present or become present in the system.
 */
void __init smp_init_cpus(void)
{
	void __iomem *scu_base = scu_base_addr();
	unsigned int i, ncores;

	ncores = scu_base ? scu_get_core_count(scu_base) : 1;

	/* sanity check */
	if (ncores > NR_CPUS) {
		printk(KERN_WARNING
		       "cns3xxx: no. of cores (%d) greater than configured "
		       "maximum of %d - clipping\n",
		       ncores, NR_CPUS);
		ncores = NR_CPUS;
	}

	for (i = 0; i < ncores; i++)
		set_cpu_possible(i, true);

	set_smp_cross_call(gic_raise_softirq);
}

void __init platform_smp_prepare_cpus(unsigned int max_cpus)
{
	int i;

	/*
	 * Initialise the present map, which describes the set of CPUs
	 * actually populated at the present time.
	 */
	for (i = 0; i < max_cpus; i++)
		set_cpu_present(i, true);

	scu_enable(scu_base_addr());

	/*
	 * Write the address of secondary startup into the
	 * system-wide flags register. The boot monitor waits
	 * until it receives a soft interrupt, and then the
	 * secondary CPU branches to this address.
	 */
	__raw_writel(virt_to_phys(cns3xxx_secondary_startup),
			(void __iomem *)(0xFFF07000 + 0x0600));

	/*
	 * Setup FIQ's for main cpu
	 */
	cns3xxx_init_fiq();
	cns3xxx_set_fiq_regs();
	memcpy((void*)&cpu_cache_save, (void*)&cpu_cache, sizeof(struct cpu_cache_fns));
}


static inline unsigned long cns3xxx_cpu_id(void)
{
	unsigned long cpu;

	asm volatile(
		" mrc p15, 0, %0, c0, c0, 5  @ cns3xxx_cpu_id\n"
		: "=r" (cpu) : : "memory", "cc");
	return (cpu & 0xf);
}

void smp_dma_map_area(const void *addr, size_t size, int dir)
{
	unsigned int cpu;
	unsigned long flags;
	raw_local_irq_save(flags);
	cpu = cns3xxx_cpu_id();
	if (cpu) {
		fiq_buffer[1] = (unsigned int)addr;
		fiq_buffer[2] = size;
		fiq_buffer[3] = dir | CNS3XXX_MAP_AREA | FIQ_ENABLED;
		smp_mb();
		__raw_writel(FIQ_GENERATE, MISC_FIQ_CPU(1));

		cpu_cache_save.dma_map_area(addr, size, dir);
		while ((fiq_buffer[3]) & FIQ_ENABLED) { barrier(); }
	} else {

		fiq_buffer[5] = (unsigned int)addr;
		fiq_buffer[6] = size;
		fiq_buffer[7] = dir | CNS3XXX_MAP_AREA | FIQ_ENABLED;
		smp_mb();
		__raw_writel(FIQ_GENERATE, MISC_FIQ_CPU(0));

		cpu_cache_save.dma_map_area(addr, size, dir);
		while ((fiq_buffer[7]) & FIQ_ENABLED) { barrier(); }
	}
	raw_local_irq_restore(flags);
}

void smp_dma_unmap_area(const void *addr, size_t size, int dir)
{
	unsigned int cpu;
	unsigned long flags;

	raw_local_irq_save(flags);
	cpu = cns3xxx_cpu_id();
	if (cpu) {

		fiq_buffer[1] = (unsigned int)addr;
		fiq_buffer[2] = size;
		fiq_buffer[3] = dir | CNS3XXX_UNMAP_AREA | FIQ_ENABLED;
		smp_mb();
		__raw_writel(FIQ_GENERATE, MISC_FIQ_CPU(1));

		cpu_cache_save.dma_unmap_area(addr, size, dir);
		while ((fiq_buffer[3]) & FIQ_ENABLED) { barrier(); }
	} else {

		fiq_buffer[5] = (unsigned int)addr;
		fiq_buffer[6] = size;
		fiq_buffer[7] = dir | CNS3XXX_UNMAP_AREA | FIQ_ENABLED;
		smp_mb();
		__raw_writel(FIQ_GENERATE, MISC_FIQ_CPU(0));

		cpu_cache_save.dma_unmap_area(addr, size, dir);
		while ((fiq_buffer[7]) & FIQ_ENABLED) { barrier(); }
	}
	raw_local_irq_restore(flags);
}

void smp_dma_flush_range(const void *start, const void *end)
{
	unsigned int cpu;
	unsigned long flags;
	raw_local_irq_save(flags);
	cpu = cns3xxx_cpu_id();
	if (cpu) {

		fiq_buffer[1] = (unsigned int)start;
		fiq_buffer[2] = (unsigned int)end;
		fiq_buffer[3] = CNS3XXX_FLUSH_RANGE | FIQ_ENABLED;
		smp_mb();
		__raw_writel(FIQ_GENERATE, MISC_FIQ_CPU(1));

		cpu_cache_save.dma_flush_range(start, end);
		while ((fiq_buffer[3]) & FIQ_ENABLED) { barrier(); }
	} else {

		fiq_buffer[5] = (unsigned int)start;
		fiq_buffer[6] = (unsigned int)end;
		fiq_buffer[7] = CNS3XXX_FLUSH_RANGE | FIQ_ENABLED;
		smp_mb();
		__raw_writel(FIQ_GENERATE, MISC_FIQ_CPU(0));

		cpu_cache_save.dma_flush_range(start, end);
		while ((fiq_buffer[7]) & FIQ_ENABLED) { barrier(); }
	}
	raw_local_irq_restore(flags);
}
