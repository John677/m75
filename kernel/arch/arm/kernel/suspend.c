#include <linux/init.h>

#include <asm/idmap.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/memory.h>
#include <asm/suspend.h>
#include <asm/tlbflush.h>

extern int __cpu_suspend(unsigned long, int (*)(unsigned long));
extern void cpu_resume_mmu(void);

#define CA15L_TYPEID 0x410FC0D0
#define CA7_TYPEID 0x410FC070
#define CPU_TYPEID_MASK 0xfffffff0

//inline int is_cpu_type(int type) 
#define read_midr()							\
	({								\
		register unsigned int ret;				\
		__asm__ __volatile__ ("mrc   p15, 0, %0, c0, c0, 0 \n\t" \
				      :"=r"(ret));			\
		ret;							\
	})

#define is_cpu_type(type)						\
	({								\
		((read_midr() & CPU_TYPEID_MASK) == type) ? 1 : 0;	\
	})

/*
 * This is called by __cpu_suspend() to save the state, and do whatever
 * flushing is required to ensure that when the CPU goes to sleep we have
 * the necessary data available when the caches are not searched.
 */
void __cpu_suspend_save(u32 *ptr, u32 ptrsz, u32 sp, u32 *save_ptr)
{
	u32 *ctx = ptr;

	*save_ptr = virt_to_phys(ptr);

	/* This must correspond to the LDM in cpu_resume() assembly */
	*ptr++ = virt_to_phys(idmap_pgd);
	*ptr++ = sp;
	*ptr++ = virt_to_phys(cpu_do_resume);

	cpu_do_suspend(ptr);

        /** optimization with CA17 CCIALL.  **/
	if (is_cpu_type(CA15L_TYPEID)) {
                __asm__ __volatile__ (
                        "mov  r0, #0 \n\t"
                        "MCR p15, 1, r0, c15, c14, 0    @; DCCIALL L1 \n\t"
                        "dsb \n\t"
                        "isb \n\t"
                        :::"r0");
        }
        else {
                flush_cache_louis();
        }

	/*
	 * flush_cache_louis does not guarantee that
	 * save_ptr and ptr are cleaned to main memory,
	 * just up to the Level of Unification Inner Shareable.
	 * Since the context pointer and context itself
	 * are to be retrieved with the MMU off that
	 * data must be cleaned from all cache levels
	 * to main memory using "area" cache primitives.
	*/
	__cpuc_flush_dcache_area(ctx, ptrsz);
	__cpuc_flush_dcache_area(save_ptr, sizeof(*save_ptr));

	outer_clean_range(*save_ptr, *save_ptr + ptrsz);
	outer_clean_range(virt_to_phys(save_ptr),
			  virt_to_phys(save_ptr) + sizeof(*save_ptr));
}

/*
 * Hide the first two arguments to __cpu_suspend - these are an implementation
 * detail which platform code shouldn't have to know about.
 */
int cpu_suspend(unsigned long arg, int (*fn)(unsigned long))
{
	struct mm_struct *mm = current->active_mm;
	int ret;

	if (!idmap_pgd)
		return -EINVAL;

	/*
	 * Provide a temporary page table with an identity mapping for
	 * the MMU-enable code, required for resuming.  On successful
	 * resume (indicated by a zero return code), we need to switch
	 * back to the correct page tables.
	 */
	ret = __cpu_suspend(arg, fn);
	if (ret == 0) {
		cpu_switch_mm(mm->pgd, mm);
		local_flush_bp_all();
		local_flush_tlb_all();
	}

	return ret;
}