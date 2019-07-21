/*
 *  linux/mm/memory.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 */

/*
 * demand-loading started 01.12.91 - seems it is high on the list of
 * things wanted, and it should be easy to implement. - Linus
 */

/*
 * Ok, demand-loading was easy, shared pages a little bit tricker. Shared
 * pages started 02.12.91, seems to work. - Linus.
 *
 * Tested sharing by executing about 30 /bin/sh: under the old kernel it
 * would have taken more than the 6M I have free, but it worked well as
 * far as I could see.
 *
 * Also corrected some "invalidate()"s - I wasn't doing enough of them.
 */

/*
 * Real VM (paging to/from disk) started 18.12.91. Much more work and
 * thought has to go into this. Oh, well..
 * 19.12.91  -  works, somewhat. Sometimes I get faults, don't know why.
 *		Found it. Everything seems to work now.
 * 20.12.91  -  Ok, making the swap-device changeable like the root.
 */

/*
 * 05.04.94  -  Multi-page memory management added for v1.1.
 * 		Idea by Alex Bligh (alex@cconcepts.co.uk)
 */

#include <linux/config.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/ptrace.h>
#include <linux/mman.h>
#include <linux/mm.h>

#include <asm/system.h>
#include <asm/segment.h>
#include <asm/pgtable.h>

unsigned long high_memory = 0;

/*
 * The free_area_list arrays point to the queue heads of the free areas
 * of different sizes
 */
int nr_swap_pages = 0;
int nr_free_pages = 0;
struct mem_list free_area_list[NR_MEM_LISTS];
unsigned char * free_area_map[NR_MEM_LISTS];

#define copy_page(from,to) memcpy((void *) to, (void *) from, PAGE_SIZE)
// 用户空间的最高级页目录项数，TASK_SIZE是用户空间的地址大小，PGDIR_SIZE是每一个最高级页目录项能管理的地址大小，这里是4M，算出共几个最高级目录项
#define USER_PTRS_PER_PGD (TASK_SIZE / PGDIR_SIZE)

mem_map_t * mem_map = NULL;

/*
 * oom() prints a message (so that the user knows why the process died),
 * and gives the process an untrappable SIGKILL.
 */
void oom(struct task_struct * task)
{
	printk("\nOut of memory for %s.\n", current->comm);
	task->sigaction[SIGKILL-1].sa_handler = NULL;
	task->blocked &= ~(1<<(SIGKILL-1));
	send_sig(SIGKILL,task,1);
}
// 释放一个页表项
static inline void free_one_pte(pte_t * page_table)
{
	pte_t page = *page_table;
	// 无效的页表项，直接返回
	if (pte_none(page))
		return;
	// 清空页表项内容，值为0
	pte_clear(page_table);
	// 如果页表项映射了物理地址，即present为1.
	if (!pte_present(page)) {
		// 
		swap_free(pte_val(page));
		return;
	}
	// 释放物理地址,pte_page得到页表项里记录的物理地址 
	free_page(pte_page(page));
	return;
}
// 释放二级页目录项和对应的页表、页表项
static inline void free_one_pmd(pmd_t * dir)
{
	int j;
	pte_t * pte;
	// 无效
	if (pmd_none(*dir))
		return;
	
	if (pmd_bad(*dir)) {
		printk("free_one_pmd: bad directory entry %08lx\n", pmd_val(*dir));
		pmd_clear(dir);
		return;
	}
	// 得到整个页表的首地址，也是第一个页表项的地址 
	pte = pte_offset(dir, 0);
	// 清除页目录项内容
	pmd_clear(dir);
	//  如果还有其他进程使用，这时候pte_free只会对pte对应的物理内存引用数减一，并且不能释放页表里的页表项
	if (pte_inuse(pte)) {
		pte_free(pte);
		return;
	}
	// 没有进程使用了，释放页表里每一个页表项
	for (j = 0; j < PTRS_PER_PTE ; j++)
		free_one_pte(pte+j);
	// 释放页表，这时候会回收物理地址，因为没人使用了
	pte_free(pte);
}
// 释放三级页目录项和相应的页表、页表项
static inline void free_one_pgd(pgd_t * dir)
{
	int j;
	pmd_t * pmd;

	if (pgd_none(*dir))
		return;
	if (pgd_bad(*dir)) {
		printk("free_one_pgd: bad directory entry %08lx\n", pgd_val(*dir));
		pgd_clear(dir);
		return;
	}
	// 取得pgd里保存的二级目录表首地址
	pmd = pmd_offset(dir, 0);
	// 清空pgd的内容
	pgd_clear(dir);
	// 如果pmd还有其他进程在使用，则pmd引用数减一即可
	if (pmd_inuse(pmd)) {
		pmd_free(pmd);
		return;
	}
	// 否则释放每一个二级目录表、目录表保存的页表、页表项
	for (j = 0; j < PTRS_PER_PMD ; j++)
		free_one_pmd(pmd+j);
	// 页目录表引用数减一
	pmd_free(pmd);
}
	

/*
 * This function clears all user-level page tables of a process - this
 * is needed by execve(), so that old pages aren't in the way. Note that
 * unlike 'free_page_tables()', this function still leaves a valid
 * page-table-tree in memory: it just removes the user pages. The two
 * functions are similar, but there is a fundamental difference.
 */
// 释放用户空间的页目录、页表
void clear_page_tables(struct task_struct * tsk)
{
	int i;
	pgd_t * page_dir;

	if (!tsk)
		return;
	// 不能释放进程0的页表
	if (tsk == task[0])
		panic("task[0] (swapper) doesn't support exec()\n");
	// 取得最高级页目录表的首地址
	page_dir = pgd_offset(tsk, 0);
	// 无效或者非法
	if (!page_dir || page_dir == swapper_pg_dir) {
		printk("%s trying to clear kernel page-directory: not good\n", tsk->comm);
		return;
	}
	// 还有进程在使用 
	if (pgd_inuse(page_dir)) {
		pgd_t * new_pg;
		// 申请一页
		if (!(new_pg = pgd_alloc())) {
			oom(tsk);
			return;
		}
		// 把用户空间的页表信息保存在新的页里
		for (i = USER_PTRS_PER_PGD ; i < PTRS_PER_PGD ; i++)
			new_pg[i] = page_dir[i];
		// 更新当前进程的最高级页目录表地址到cr3
		SET_PAGE_DIR(tsk, new_pg);
		// 最高级页目录表对应物理地址引用数减一
		pgd_free(page_dir);
		return;
	}
	// 释放每一个最高级的页目录项
	for (i = 0 ; i < USER_PTRS_PER_PGD ; i++)
		free_one_pgd(page_dir + i);
	// 刷新快表，使得对应的信息无效
	invalidate();
	return;
}

/*
 * This function frees up all page tables of a process when it exits.
 */
// 释放进程的所有页目录、页表
void free_page_tables(struct task_struct * tsk)
{
	int i;
	pgd_t * page_dir;

	if (!tsk)
		return;
	if (tsk == task[0]) {
		printk("task[0] (swapper) killed: unable to recover\n");
		panic("Trying to free up swapper memory space");
	}
	// 最高级页目录表首地址
	page_dir = pgd_offset(tsk, 0);
	if (!page_dir || page_dir == swapper_pg_dir) {
		printk("%s trying to free kernel page-directory: not good\n", tsk->comm);
		return;
	}
	// 更新进程的cr3字段
	SET_PAGE_DIR(tsk, swapper_pg_dir);
	// 对应的物理地址还有其他进程使用，则引用数减一
	if (pgd_inuse(page_dir)) {
		pgd_free(page_dir);
		return;
	}
	// 没有被使用了，释放全部页表信息
	for (i = 0 ; i < PTRS_PER_PGD ; i++)
		free_one_pgd(page_dir + i);
	// 释放pgd对应的物理内存
	pgd_free(page_dir);
	// 刷新快表
	invalidate();
}

/*
 * clone_page_tables() clones the page table for a process - both
 * processes will have the exact same pages in memory. There are
 * probably races in the memory management with cloning, but we'll
 * see..
 */
// 复制页表信息
int clone_page_tables(struct task_struct * tsk)
{
	pgd_t * pg_dir;
	// 取得当前进程最高级页目录表的地址
	pg_dir = pgd_offset(current, 0);
	// 引用数加一 
	pgd_reuse(pg_dir);
	// 设置进程的cr3位新的页目录表地址
	SET_PAGE_DIR(tsk, pg_dir);
	return 0;
}
// 复制一个页表项
static inline void copy_one_pte(pte_t * old_pte, pte_t * new_pte)
{
	pte_t pte = *old_pte;
	// 有效性判断
	if (pte_none(pte))
		return;
	// 页表项没有映射到物理页
	if (!pte_present(pte)) {
		swap_duplicate(pte_val(pte));
		*new_pte = pte;
		return;
	}
	if (pte_page(pte) > high_memory || (mem_map[MAP_NR(pte_page(pte))] & MAP_PAGE_RESERVED)) {
		*new_pte = pte;
		return;
	}
	// 
	if (pte_cow(pte))
		pte = pte_wrprotect(pte);
	if (delete_from_swap_cache(pte_page(pte)))
		pte = pte_mkdirty(pte);
	*new_pte = pte_mkold(pte);
	*old_pte = pte;
	mem_map[MAP_NR(pte_page(pte))]++;
}
// 复制一个耳机页目录项
static inline int copy_one_pmd(pmd_t * old_pmd, pmd_t * new_pmd)
{
	int j;
	pte_t *old_pte, *new_pte;

	if (pmd_none(*old_pmd))
		return 0;
	if (pmd_bad(*old_pmd)) {
		printk("copy_one_pmd: bad page table: probable memory corruption\n");
		pmd_clear(old_pmd);
		return 0;
	}
	// 取得该页目录项对应的页表首地址
	old_pte = pte_offset(old_pmd, 0);
	// 页表有其他进程使用
	if (pte_inuse(old_pte)) {
		// 引用数加一
		pte_reuse(old_pte);
		// 复制页表首地址到页目录项里
		*new_pmd = *old_pmd;
		return 0;
	}
	// 在页目录项中取得页表首地址，然后返回第一个页表项地址，如果new_pmd是空，则分配新的一个，new_pmd指向新页的地址
	new_pte = pte_alloc(new_pmd, 0);
	if (!new_pte)
		return -ENOMEM;
	// 复制每一个页表项，即复制页表
	for (j = 0 ; j < PTRS_PER_PTE ; j++) {
		copy_one_pte(old_pte, new_pte);
		old_pte++;
		new_pte++;
	}
	return 0;
}
// 同上
static inline int copy_one_pgd(pgd_t * old_pgd, pgd_t * new_pgd)
{
	int j;
	pmd_t *old_pmd, *new_pmd;

	if (pgd_none(*old_pgd))
		return 0;
	if (pgd_bad(*old_pgd)) {
		printk("copy_one_pgd: bad page table (%p: %08lx): probable memory corruption\n", old_pgd, pgd_val(*old_pgd));
		pgd_clear(old_pgd);
		return 0;
	}
	old_pmd = pmd_offset(old_pgd, 0);
	if (pmd_inuse(old_pmd)) {
		pmd_reuse(old_pmd);
		*new_pgd = *old_pgd;
		return 0;
	}
	new_pmd = pmd_alloc(new_pgd, 0);
	if (!new_pmd)
		return -ENOMEM;
	for (j = 0 ; j < PTRS_PER_PMD ; j++) {
		int error = copy_one_pmd(old_pmd, new_pmd);
		if (error)
			return error;
		old_pmd++;
		new_pmd++;
	}
	return 0;
}

/*
 * copy_page_tables() just copies the whole process memory range:
 * note the special handling of RESERVED (ie kernel) pages, which
 * means that they are always shared by all processes.
 */
// 复制页表信息
int copy_page_tables(struct task_struct * tsk)
{
	int i;
	pgd_t *old_pgd;
	pgd_t *new_pgd;
	// 分配一页
	new_pgd = pgd_alloc();
	if (!new_pgd)
		return -ENOMEM;
	// 设置进程的cr3字段，即最高级页目录表首地址
	SET_PAGE_DIR(tsk, new_pgd);
	// 取得当前进程的最高级页目录表首地址
	old_pgd = pgd_offset(current, 0);
	// 复制每一项
	for (i = 0 ; i < PTRS_PER_PGD ; i++) {
		int errno = copy_one_pgd(old_pgd, new_pgd);
		if (errno) {
			free_page_tables(tsk);
			invalidate();
			return errno;
		}
		old_pgd++;
		new_pgd++;
	}
	invalidate();
	return 0;
}

static inline void forget_pte(pte_t page)
{
	if (pte_none(page))
		return;
	if (pte_present(page)) {
		free_page(pte_page(page));
		if (mem_map[MAP_NR(pte_page(page))] & MAP_PAGE_RESERVED)
			return;
		if (current->mm->rss <= 0)
			return;
		current->mm->rss--;
		return;
	}
	swap_free(pte_val(page));
}

static inline void unmap_pte_range(pmd_t * pmd, unsigned long address, unsigned long size)
{
	pte_t * pte;
	unsigned long end;

	if (pmd_none(*pmd))
		return;
	if (pmd_bad(*pmd)) {
		printk("unmap_pte_range: bad pmd (%08lx)\n", pmd_val(*pmd));
		pmd_clear(pmd);
		return;
	}
	// 取得address对应的页表项首地址,address是虚拟内存，里面保存了该页表项的偏移
	pte = pte_offset(pmd, address);
	// 取得物理页内偏移
	address &= ~PMD_MASK;
	// 物理地址的结束地址
	end = address + size;
	// 是否超出了该页目录项管理的内存大小
	if (end >= PMD_SIZE)
		end = PMD_SIZE;
	do {
		// 页表项内容
		pte_t page = *pte;
		// 清空
		pte_clear(pte);
		forget_pte(page);
		address += PAGE_SIZE;
		pte++;
	} while (address < end);
}

static inline void unmap_pmd_range(pgd_t * dir, unsigned long address, unsigned long size)
{
	pmd_t * pmd;
	unsigned long end;

	if (pgd_none(*dir))
		return;
	if (pgd_bad(*dir)) {
		printk("unmap_pmd_range: bad pgd (%08lx)\n", pgd_val(*dir));
		pgd_clear(dir);
		return;
	}
	pmd = pmd_offset(dir, address);
	address &= ~PGDIR_MASK;
	end = address + size;
	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;
	do {
		unmap_pte_range(pmd, address, end - address);
		address = (address + PMD_SIZE) & PMD_MASK; 
		pmd++;
	} while (address < end);
}

/*
 * a more complete version of free_page_tables which performs with page
 * granularity.
 */
int unmap_page_range(unsigned long address, unsigned long size)
{
	pgd_t * dir;
	unsigned long end = address + size;

	dir = pgd_offset(current, address);
	while (address < end) {
		unmap_pmd_range(dir, address, end - address);
		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		dir++;
	}
	invalidate();
	return 0;
}

static inline void zeromap_pte_range(pte_t * pte, unsigned long address, unsigned long size, pte_t zero_pte)
{
	unsigned long end;

	address &= ~PMD_MASK;
	end = address + size;
	if (end > PMD_SIZE)
		end = PMD_SIZE;
	do {
		pte_t oldpage = *pte;
		*pte = zero_pte;
		forget_pte(oldpage);
		address += PAGE_SIZE;
		pte++;
	} while (address < end);
}

static inline int zeromap_pmd_range(pmd_t * pmd, unsigned long address, unsigned long size, pte_t zero_pte)
{
	unsigned long end;

	address &= ~PGDIR_MASK;
	end = address + size;
	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;
	do {
		pte_t * pte = pte_alloc(pmd, address);
		if (!pte)
			return -ENOMEM;
		zeromap_pte_range(pte, address, end - address, zero_pte);
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address < end);
	return 0;
}

int zeromap_page_range(unsigned long address, unsigned long size, pgprot_t prot)
{
	int error = 0;
	pgd_t * dir;
	unsigned long end = address + size;
	pte_t zero_pte;

	zero_pte = pte_wrprotect(mk_pte(ZERO_PAGE, prot));
	dir = pgd_offset(current, address);
	while (address < end) {
		pmd_t *pmd = pmd_alloc(dir, address);
		error = -ENOMEM;
		if (!pmd)
			break;
		error = zeromap_pmd_range(pmd, address, end - address, zero_pte);
		if (error)
			break;
		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		dir++;
	}
	invalidate();
	return error;
}

/*
 * maps a range of physical memory into the requested pages. the old
 * mappings are removed. any references to nonexistent pages results
 * in null mappings (currently treated as "copy-on-access")
 */
static inline void remap_pte_range(pte_t * pte, unsigned long address, unsigned long size,
	unsigned long offset, pgprot_t prot)
{
	unsigned long end;

	address &= ~PMD_MASK;
	end = address + size;
	if (end > PMD_SIZE)
		end = PMD_SIZE;
	do {
		pte_t oldpage = *pte;
		pte_clear(pte);
		if (offset >= high_memory || (mem_map[MAP_NR(offset)] & MAP_PAGE_RESERVED))
			*pte = mk_pte(offset, prot);
		else if (mem_map[MAP_NR(offset)]) {
			mem_map[MAP_NR(offset)]++;
			*pte = mk_pte(offset, prot);
		}
		forget_pte(oldpage);
		address += PAGE_SIZE;
		offset += PAGE_SIZE;
		pte++;
	} while (address < end);
}

static inline int remap_pmd_range(pmd_t * pmd, unsigned long address, unsigned long size,
	unsigned long offset, pgprot_t prot)
{
	unsigned long end;

	address &= ~PGDIR_MASK;
	end = address + size;
	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;
	offset -= address;
	do {
		pte_t * pte = pte_alloc(pmd, address);
		if (!pte)
			return -ENOMEM;
		remap_pte_range(pte, address, end - address, address + offset, prot);
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address < end);
	return 0;
}

int remap_page_range(unsigned long from, unsigned long offset, unsigned long size, pgprot_t prot)
{
	int error = 0;
	pgd_t * dir;
	unsigned long end = from + size;

	offset -= from;
	dir = pgd_offset(current, from);
	while (from < end) {
		pmd_t *pmd = pmd_alloc(dir, from);
		error = -ENOMEM;
		if (!pmd)
			break;
		error = remap_pmd_range(pmd, from, end - from, offset + from, prot);
		if (error)
			break;
		from = (from + PGDIR_SIZE) & PGDIR_MASK;
		dir++;
	}
	invalidate();
	return error;
}

/*
 * sanity-check function..
 */
// 复制页表项内容
static void put_page(pte_t * page_table, pte_t pte)
{	
	// 页表项已经保存了映射信息
	if (!pte_none(*page_table)) {
		printk("put_page: page already exists %08lx\n", pte_val(*page_table));
		free_page(pte_page(pte));
		return;
	}
/* no need for invalidate */
	// 复制
	*page_table = pte;
}

/*
 * This routine is used to map in a page into an address space: needed by
 * execve() for the initial stack and environment pages.
 */
// 把address对应的物理地址写入页表。address是虚拟地址，page是物理地址
unsigned long put_dirty_page(struct task_struct * tsk, unsigned long page, unsigned long address)
{
	pgd_t * pgd;
	pmd_t * pmd;
	pte_t * pte;

	if (page >= high_memory)
		printk("put_dirty_page: trying to put page %08lx at %08lx\n",page,address);
	// 该物理地址没有标记为使用，所以不能映射
	if (mem_map[MAP_NR(page)] != 1)
		printk("mem_map disagrees with %08lx at %08lx\n",page,address);
	// 取得address对应的最高级目录表中的一个项
	pgd = pgd_offset(tsk,address);
	// 取得二级目录表中的某个项，即页表首地址，pgd为空则分配新页，pgd指向新页
	pmd = pmd_alloc(pgd, address);
	if (!pmd) {
		free_page(page);
		oom(tsk);
		return 0;
	}
	// 同上，取得一个页表项首地址
	pte = pte_alloc(pmd, address);
	if (!pte) {
		free_page(page);
		oom(tsk);
		return 0;
	}
	// 页表项已经有信息了
	if (!pte_none(*pte)) {
		printk("put_dirty_page: page already exists\n");
		pte_clear(pte);
		invalidate();
	}
	// 填充页表项里的内容，包括dirty，可读写执行，PAGE_COPY看定义
	*pte = pte_mkwrite(pte_mkdirty(mk_pte(page, PAGE_COPY)));
/* no need for invalidate */
	return page;
}

/*
 * This routine handles present pages, when users try to write
 * to a shared page. It is done by copying the page to a new address
 * and decrementing the shared-page counter for the old page.
 *
 * Goto-purists beware: the only reason for goto's here is that it results
 * in better assembly code.. The "default" path will see no jumps at all.
 *
 * Note that this routine assumes that the protection checks have been
 * done by the caller (the low-level page fault routine in most cases).
 * Thus we can safely just mark it writable once we've done any necessary
 * COW.
 *
 * We also mark the page dirty at this point even though the page will
 * change only once the write actually happens. This avoids a few races,
 * and potentially makes it more efficient.
 */
// 用户写多个进程共用的页时，需要申请一个新页，然后把旧页的数据复制过来，修改页表项
void do_wp_page(struct vm_area_struct * vma, unsigned long address,
	int write_access)
{
	pgd_t *page_dir;
	pmd_t *page_middle;
	pte_t *page_table, pte;
	unsigned long old_page, new_page;
	// 获取一页物理页
	new_page = __get_free_page(GFP_KERNEL);
	// 获取进程最高级页目录项
	page_dir = pgd_offset(vma->vm_task,address);
	if (pgd_none(*page_dir))
		goto end_wp_page;
	if (pgd_bad(*page_dir))
		goto bad_wp_pagedir;
	// 获取对应的二级目录项
	page_middle = pmd_offset(page_dir, address);
	if (pmd_none(*page_middle))
		goto end_wp_page;
	if (pmd_bad(*page_middle))
		goto bad_wp_pagemiddle;
	// 获取页表项
	page_table = pte_offset(page_middle, address);
	pte = *page_table;
	// 没有映射物理内存
	if (!pte_present(pte))
		goto end_wp_page;
	// 可写，则不需要处理
	if (pte_write(pte))
		goto end_wp_page;
	// 获取对应的物理地址 
	old_page = pte_page(pte);
	if (old_page >= high_memory)
		goto bad_wp_page;
	vma->vm_task->mm->min_flt++;
	/*
	 * Do we need to copy?
	 */ 
	// 如果有多个进程在使用，则需要为执行写操作的进程，新增一个页表项，指向新页，如果只有一个进程在使用，则不需要
	if (mem_map[MAP_NR(old_page)] != 1) {
		if (new_page) {
			if (mem_map[MAP_NR(old_page)] & MAP_PAGE_RESERVED)
				++vma->vm_task->mm->rss;
			// 把物理页里的内容复制到新页
			copy_page(old_page,new_page);
			// 标记新页可读写执行，dirty等
			*page_table = pte_mkwrite(pte_mkdirty(mk_pte(new_page, vma->vm_page_prot)));
			// 老页引用数减一
			free_page(old_page);
			// 刷新快表
			invalidate();
			return;
		}
		// 申请新页失败，标记页表项为失败
		*page_table = BAD_PAGE;
		// 老页引用数减一
		free_page(old_page);
		oom(vma->vm_task);
		invalidate();
		return;
	}
	// 打标记
	*page_table = pte_mkdirty(pte_mkwrite(pte));
	// 刷新快表
	invalidate();
	// 释放申请的新页，因为没有用到
	if (new_page)
		free_page(new_page);
	return;
bad_wp_page:
	printk("do_wp_page: bogus page at address %08lx (%08lx)\n",address,old_page);
	send_sig(SIGKILL, vma->vm_task, 1);
	goto end_wp_page;
bad_wp_pagemiddle:
	printk("do_wp_page: bogus page-middle at address %08lx (%08lx)\n", address, pmd_val(*page_middle));
	send_sig(SIGKILL, vma->vm_task, 1);
	goto end_wp_page;
bad_wp_pagedir:
	printk("do_wp_page: bogus page-dir entry at address %08lx (%08lx)\n", address, pgd_val(*page_dir));
	send_sig(SIGKILL, vma->vm_task, 1);
end_wp_page:
	if (new_page)
		free_page(new_page);
	return;
}

/*
 * Ugly, ugly, but the goto's result in better assembly..
 */
int verify_area(int type, const void * addr, unsigned long size)
{
	struct vm_area_struct * vma;
	unsigned long start = (unsigned long) addr;

	/* If the current user space is mapped to kernel space (for the
	 * case where we use a fake user buffer with get_fs/set_fs()) we
	 * don't expect to find the address in the user vm map.
	 */
	if (get_fs() == get_ds())
		return 0;

	vma = find_vma(current, start);
	if (!vma)
		goto bad_area;
	if (vma->vm_start <= start)
		goto good_area;
	if (!(vma->vm_flags & VM_GROWSDOWN))
		goto bad_area;
	if (vma->vm_end - start > current->rlim[RLIMIT_STACK].rlim_cur)
		goto bad_area;

good_area:
	if (type == VERIFY_WRITE)
		goto check_write;
	for (;;) {
		struct vm_area_struct * next;
		if (!(vma->vm_flags & VM_READ))
			goto bad_area;
		if (vma->vm_end - start >= size)
			return 0;
		next = vma->vm_next;
		if (!next || vma->vm_end != next->vm_start)
			goto bad_area;
		vma = next;
	}

check_write:
	if (!(vma->vm_flags & VM_WRITE))
		goto bad_area;
	if (!wp_works_ok)
		goto check_wp_fault_by_hand;
	for (;;) {
		if (vma->vm_end - start >= size)
			break;
		if (!vma->vm_next || vma->vm_end != vma->vm_next->vm_start)
			goto bad_area;
		vma = vma->vm_next;
		if (!(vma->vm_flags & VM_WRITE))
			goto bad_area;
	}
	return 0;

check_wp_fault_by_hand:
	size--;
	size += start & ~PAGE_MASK;
	size >>= PAGE_SHIFT;
	start &= PAGE_MASK;

	for (;;) {
		do_wp_page(vma, start, 1);
		if (!size)
			break;
		size--;
		start += PAGE_SIZE;
		if (start < vma->vm_end)
			continue;
		vma = vma->vm_next;
		if (!vma || vma->vm_start != start)
			goto bad_area;
		if (!(vma->vm_flags & VM_WRITE))
			goto bad_area;;
	}
	return 0;

bad_area:
	return -EFAULT;
}
// 获取一个新的物理页并记录到页表项里
static inline void get_empty_page(struct vm_area_struct * vma, pte_t * page_table)
{
	unsigned long tmp;
	// 申请失败则标记失败
	if (!(tmp = get_free_page(GFP_KERNEL))) {
		oom(vma->vm_task);
		put_page(page_table, BAD_PAGE);
		return;
	}
	// 建立虚拟地址到物理地址的映射
	put_page(page_table, pte_mkwrite(mk_pte(tmp, vma->vm_page_prot)));
}

/*
 * try_to_share() checks the page at address "address" in the task "p",
 * to see if it exists, and if it is clean. If so, share it with the current
 * task.
 *
 * NOTE! This assumes we have checked that p != current, and that they
 * share the same inode and can generally otherwise be shared.
 */
static int try_to_share(unsigned long to_address, struct vm_area_struct * to_area,
	unsigned long from_address, struct vm_area_struct * from_area,
	unsigned long newpage)
{
	pgd_t * from_dir, * to_dir;
	pmd_t * from_middle, * to_middle;
	pte_t * from_table, * to_table;
	pte_t from, to;
	// 获取最高级页目录项
	from_dir = pgd_offset(from_area->vm_task,from_address);
/* is there a page-directory at from? */
	// 不存在
	if (pgd_none(*from_dir))
		return 0;
	if (pgd_bad(*from_dir)) {
		printk("try_to_share: bad page directory %08lx\n", pgd_val(*from_dir));
		pgd_clear(from_dir);
		return 0;
	}
	// 获取二级页目录项
	from_middle = pmd_offset(from_dir, from_address);
/* is there a mid-directory at from? */
	if (pmd_none(*from_middle))
		return 0;
	if (pmd_bad(*from_middle)) {
		printk("try_to_share: bad mid directory %08lx\n", pmd_val(*from_middle));
		pmd_clear(from_middle);
		return 0;
	}
	// 获取页表项
	from_table = pte_offset(from_middle, from_address);
	from = *from_table;
/* is the page present? */
	// 没有映射到物理地址
	if (!pte_present(from))
		return 0;
/* if it is dirty it must be from a shared mapping to be shared */
	// 脏页
	if (pte_dirty(from)) {
		// 这块内存不可共享
		if (!(from_area->vm_flags & VM_SHARED))
			return 0;
		// 这块内存可写，不能分享
		if (pte_write(from)) {
			printk("nonwritable, but dirty, shared page\n");
			return 0;
		}
	}
/* is the page reasonable at all? */
	if (pte_page(from) >= high_memory)
		return 0;
	// 物理地址是保留地址
	if (mem_map[MAP_NR(pte_page(from))] & MAP_PAGE_RESERVED)
		return 0;
/* is the destination ok? */
	// 目的地址是否有效，目的地址是需要共享这块内存的另一个进程
	to_dir = pgd_offset(to_area->vm_task,to_address);
/* is there a page-directory at to? */
	if (pgd_none(*to_dir))
		return 0;
	if (pgd_bad(*to_dir)) {
		printk("try_to_share: bad page directory %08lx\n", pgd_val(*to_dir));
		return 0;
	}
	to_middle = pmd_offset(to_dir, to_address);
/* is there a mid-directory at to? */
	if (pmd_none(*to_middle))
		return 0;
	if (pmd_bad(*to_middle)) {
		printk("try_to_share: bad mid directory %08lx\n", pmd_val(*to_middle));
		return 0;
	}
	to_table = pte_offset(to_middle, to_address);
	to = *to_table;
	if (!pte_none(to))
		return 0;
/* do we copy? */
	if (newpage) {
		/* if it's in the swap cache, it's dirty by implication */
		/* so we can't use it if it's not from a shared mapping */
		if (in_swap_cache(pte_page(from))) {
			if (!(from_area->vm_flags & VM_SHARED))
				return 0;
			if (!pte_write(from)) {
				printk("nonwritable, but dirty, shared page\n");
				return 0;
			}
		}
		// 复制物理页内容到新页
		copy_page(pte_page(from), newpage);
		// 修改页表项内容是新页的地址
		*to_table = mk_pte(newpage, to_area->vm_page_prot);
		return 1;
	}
/*
 * do a final swap-cache test before sharing them: if it's in the swap
 * cache, we have to remove it now, as we get two pointers to the same
 * physical page and the cache can't handle it. Mark the original dirty.
 *
 * NOTE! Even if "from" is dirty, "to" will be clean: if we get here
 * with a dirty "from", the from-mapping is a shared map, so we can trust
 * the page contents to be up-to-date
 */
	if (in_swap_cache(pte_page(from))) {
		if (!(from_area->vm_flags & VM_SHARED))
			return 0;
		*from_table = pte_mkdirty(from);
		delete_from_swap_cache(pte_page(from));
	}
	// 物理地址引用数加一
	mem_map[MAP_NR(pte_page(from))]++;
	// 更新页表项
	*to_table = mk_pte(pte_page(from), to_area->vm_page_prot);
/* Check if we need to do anything at all to the 'from' field */
	if (!pte_write(from))
		return 1;
	if (from_area->vm_flags & VM_SHARED)
		return 1;
/* ok, need to mark it read-only, so invalidate any possible old TB entry */
	*from_table = pte_wrprotect(from);
	invalidate();
	return 1;
}

/*
 * share_page() tries to find a process that could share a page with
 * the current one.
 *
 * We first check if it is at all feasible by checking inode->i_count.
 * It should be >1 if there are other tasks sharing this inode.
 */
static int share_page(struct vm_area_struct * area, unsigned long address,
	int write_access, unsigned long newpage)
{
	struct inode * inode;
	unsigned long offset;
	unsigned long from_address;
	unsigned long give_page;
	struct vm_area_struct * mpnt;

	if (!area || !(inode = area->vm_inode) || inode->i_count < 2)
		return 0;
	/* do we need to copy or can we just share? */
	give_page = 0;
	if (write_access && !(area->vm_flags & VM_SHARED)) {
		if (!newpage)
			return 0;
		give_page = newpage;
	}
	offset = address - area->vm_start + area->vm_offset;
	/* See if there is something in the VM we can share pages with. */
	/* Traverse the entire circular i_mmap list, except `area' itself. */
	for (mpnt = area->vm_next_share; mpnt != area; mpnt = mpnt->vm_next_share) {
		/* must be same inode */
		if (mpnt->vm_inode != inode) {
			printk("Aiee! Corrupt vm_area_struct i_mmap ring\n");
			break;	
		}
		/* offsets must be mutually page-aligned */
		if ((mpnt->vm_offset ^ area->vm_offset) & ~PAGE_MASK)
			continue;
		/* the other area must actually cover the wanted page.. */
		from_address = offset + mpnt->vm_start - mpnt->vm_offset;
		if (from_address < mpnt->vm_start || from_address >= mpnt->vm_end)
			continue;
		/* .. NOW we can actually try to use the same physical page */
		if (!try_to_share(address, area, from_address, mpnt, give_page))
			continue;
		/* free newpage if we never used it.. */
		if (give_page || !newpage)
			return 1;
		free_page(newpage);
		return 1;
	}
	return 0;
}

/*
 * fill in an empty page-table if none exists.
 */
// 获取虚拟地址对应的页表项首地址
static inline pte_t * get_empty_pgtable(struct task_struct * tsk,unsigned long address)
{
	pgd_t *pgd;
	pmd_t *pmd;
	pte_t *pte;
	// 逐级获取页目录信息
	pgd = pgd_offset(tsk, address);
	pmd = pmd_alloc(pgd, address);
	if (!pmd) {
		oom(tsk);
		return NULL;
	}
	// 获取页表项
	pte = pte_alloc(pmd, address);
	if (!pte) {
		oom(tsk);
		return NULL;
	}
	return pte;
}

static inline void do_swap_page(struct vm_area_struct * vma, unsigned long address,
	pte_t * page_table, pte_t entry, int write_access)
{
	pte_t page;

	if (!vma->vm_ops || !vma->vm_ops->swapin) {
		swap_in(vma, page_table, pte_val(entry), write_access);
		return;
	}
	page = vma->vm_ops->swapin(vma, address - vma->vm_start + vma->vm_offset, pte_val(entry));
	if (pte_val(*page_table) != pte_val(entry)) {
		free_page(pte_page(page));
		return;
	}
	if (mem_map[MAP_NR(pte_page(page))] > 1 && !(vma->vm_flags & VM_SHARED))
		page = pte_wrprotect(page);
	++vma->vm_task->mm->rss;
	++vma->vm_task->mm->maj_flt;
	*page_table = page;
	return;
}

/*
 * do_no_page() tries to create a new page mapping. It aggressively
 * tries to share with existing pages, but makes a separate copy if
 * the "write_access" parameter is true in order to avoid the next
 * page fault.
 */
void do_no_page(struct vm_area_struct * vma, unsigned long address,
	int write_access)
{
	pte_t * page_table;
	pte_t entry;
	unsigned long page;

	page_table = get_empty_pgtable(vma->vm_task,address);
	if (!page_table)
		return;
	entry = *page_table;
	if (pte_present(entry))
		return;
	if (!pte_none(entry)) {
		do_swap_page(vma, address, page_table, entry, write_access);
		return;
	}
	address &= PAGE_MASK;
	if (!vma->vm_ops || !vma->vm_ops->nopage) {
		++vma->vm_task->mm->rss;
		++vma->vm_task->mm->min_flt;
		get_empty_page(vma, page_table);
		return;
	}
	page = get_free_page(GFP_KERNEL);
	if (share_page(vma, address, write_access, page)) {
		++vma->vm_task->mm->min_flt;
		++vma->vm_task->mm->rss;
		return;
	}
	if (!page) {
		oom(current);
		put_page(page_table, BAD_PAGE);
		return;
	}
	++vma->vm_task->mm->maj_flt;
	++vma->vm_task->mm->rss;
	/*
	 * The fourth argument is "no_share", which tells the low-level code
	 * to copy, not share the page even if sharing is possible.  It's
	 * essentially an early COW detection 
	 */
	page = vma->vm_ops->nopage(vma, address, page,
		write_access && !(vma->vm_flags & VM_SHARED));
	if (share_page(vma, address, write_access, 0)) {
		free_page(page);
		return;
	}
	/*
	 * This silly early PAGE_DIRTY setting removes a race
	 * due to the bad i386 page protection. But it's valid
	 * for other architectures too.
	 *
	 * Note that if write_access is true, we either now have
	 * a exclusive copy of the page, or this is a shared mapping,
	 * so we can make it writable and dirty to avoid having to
	 * handle that later.
	 */
	entry = mk_pte(page, vma->vm_page_prot);
	if (write_access) {
		entry = pte_mkwrite(pte_mkdirty(entry));
	} else if (mem_map[MAP_NR(page)] > 1 && !(vma->vm_flags & VM_SHARED))
		entry = pte_wrprotect(entry);
	put_page(page_table, entry);
}

/*
 * The above separate functions for the no-page and wp-page
 * cases will go away (they mostly do the same thing anyway),
 * and we'll instead use only a general "handle_mm_fault()".
 *
 * These routines also need to handle stuff like marking pages dirty
 * and/or accessed for architectures that don't do it in hardware (most
 * RISC architectures).  The early dirtying is also good on the i386.
 *
 * There is also a hook called "update_mmu_cache()" that architectures
 * with external mmu caches can use to update those (ie the Sparc or
 * PowerPC hashed page tables that act as extended TLBs).
 */
static inline void handle_pte_fault(struct vm_area_struct * vma, unsigned long address,
	int write_access, pte_t * pte)
{
	if (!pte_present(*pte)) {
		do_no_page(vma, address, write_access);
		return;
	}
	*pte = pte_mkyoung(*pte);
	if (!write_access)
		return;
	if (pte_write(*pte)) {
		*pte = pte_mkdirty(*pte);
		return;
	}
	do_wp_page(vma, address, write_access);
}

void handle_mm_fault(struct vm_area_struct * vma, unsigned long address,
	int write_access)
{
	pgd_t *pgd;
	pmd_t *pmd;
	pte_t *pte;

	pgd = pgd_offset(vma->vm_task, address);
	pmd = pmd_alloc(pgd, address);
	if (!pmd)
		goto no_memory;
	pte = pte_alloc(pmd, address);
	if (!pte)
		goto no_memory;
	handle_pte_fault(vma, address, write_access, pte);
	update_mmu_cache(vma, address, *pte);
	return;
no_memory:
	oom(vma->vm_task);
}
