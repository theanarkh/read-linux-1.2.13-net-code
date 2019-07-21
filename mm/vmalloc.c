/*
 *  linux/mm/vmalloc.c
 *
 *  Copyright (C) 1993  Linus Torvalds
 */

#include <asm/system.h>

#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/malloc.h>
#include <linux/mm.h>

#include <asm/segment.h>
#include <asm/pgtable.h>

struct vm_struct {
	unsigned long flags;
	void * addr;
	unsigned long size;
	struct vm_struct * next;
};

static struct vm_struct * vmlist = NULL;
// 设置每个进程的最高级页目录项内容为entry
static inline void set_pgdir(unsigned long address, pgd_t entry)
{
	struct task_struct * p;

	for_each_task(p)
		*pgd_offset(p,address) = entry;
}
// 释放某个虚拟内存区间对应的物理页，address是虚拟地址首地址，size是大小
static inline void free_area_pte(pmd_t * pmd, unsigned long address, unsigned long size)
{
	pte_t * pte;
	unsigned long end;

	if (pmd_none(*pmd))
		return;
	if (pmd_bad(*pmd)) {
		printk("free_area_pte: bad pmd (%08lx)\n", pmd_val(*pmd));
		pmd_clear(pmd);
		return;
	}
	// 页表项地址
	pte = pte_offset(pmd, address);
	// 物理页内偏移
	address &= ~PMD_MASK;
	// 结束地址
	end = address + size;
	// 超过了页目录管理的范围，只取管理范围的
	if (end > PMD_SIZE)
		end = PMD_SIZE;
	// 从虚拟地址address到end逐页释放
	while (address < end) {
		pte_t page = *pte;
		// 清除页表项内容
		pte_clear(pte);
		// 下一个要处理的地址，即下一页
		address += PAGE_SIZE;
		// 指向下一个页表项
		pte++;
		// 无效则跳过
		if (pte_none(page))
			continue;
		// 有效则引用数加一，没进程使用则释放
		if (pte_present(page)) {
			free_page(pte_page(page));
			continue;
		}
		printk("Whee.. Swapped out page in kernel page table\n");
	}
}
// 释放address到address+size内的虚拟地址对应的页表信息
static inline void free_area_pmd(pgd_t * dir, unsigned long address, unsigned long size)
{
	pmd_t * pmd;
	unsigned long end;

	if (pgd_none(*dir))
		return;
	if (pgd_bad(*dir)) {
		printk("free_area_pmd: bad pgd (%08lx)\n", pgd_val(*dir));
		pgd_clear(dir);
		return;
	}
	// 获取二级目录表首地址
	pmd = pmd_offset(dir, address);
	// 屏蔽最高级页目录地址的位
	address &= ~PGDIR_MASK;
	// 释放的末虚拟地址
	end = address + size;
	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;
	/*
		逐个页目录项释放，pmd保存了一个页表的首地址，
		address代表从页表的哪个页表项开始释放，end-address
		保证了从某项开始一直释放到最后一个,free_area_pte保证只释放合法的地址
	*/
	while (address < end) {
		free_area_pte(pmd, address, end - address);
		// 计算出下一个页目录项的地址，下一轮释放。(address + PMD_SIZE) 得到下一个页目录项地址，(address + PMD_SIZE) & PMD_MASK得到页表首地址
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	}
}

static void free_area_pages(unsigned long address, unsigned long size)
{
	pgd_t * dir;
	unsigned long end = address + size;

	dir = pgd_offset(&init_task, address);
	while (address < end) {
		free_area_pmd(dir, address, end - address);
		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		dir++;
	}
	invalidate();
}
// 给虚拟地址address到address+size的虚拟地址映射物理地址，写入页表中
static inline int alloc_area_pte(pte_t * pte, unsigned long address, unsigned long size)
{
	unsigned long end;
	// 取得目录项和页表、页内偏移部分的内容
	address &= ~PMD_MASK;
	end = address + size;
	if (end > PMD_SIZE)
		end = PMD_SIZE;
	while (address < end) {
		unsigned long page;
		// 非空则不能再赋值
		if (!pte_none(*pte))
			printk("alloc_area_pte: page already exists\n");
		page = __get_free_page(GFP_KERNEL);
		if (!page)
			return -ENOMEM;
		// 把申请到的地址写入页表项
		*pte = mk_pte(page, PAGE_KERNEL);
		// 下一页
		address += PAGE_SIZE;
		// 下一个需要写入的页表项
		pte++;
	}
	return 0;
}
// 给address到address+size之间的地址建立页目录表、页表信息
static inline int alloc_area_pmd(pmd_t * pmd, unsigned long address, unsigned long size)
{
	unsigned long end;
	// 屏蔽高位，得到有效位 
	address &= ~PGDIR_MASK;
	end = address + size;
	if (end > PGDIR_SIZE)
		end = PGDIR_SIZE;
	while (address < end) {
		// 在页目录表中根据address的值算出页表首地址，pmd指向页目录表首地址
		pte_t * pte = pte_alloc_kernel(pmd, address);
		if (!pte)
			return -ENOMEM;
		// 在页表中建立虚拟地址和物理地址的映射
		if (alloc_area_pte(pte, address, end - address))
			return -ENOMEM;
		// 处理下一个页目录
		address = (address + PMD_SIZE) & PMD_MASK;
		// 下一个页目录表地址
		pmd++;
	}
	return 0;
}
// 同上
static int alloc_area_pages(unsigned long address, unsigned long size)
{
	pgd_t * dir;
	unsigned long end = address + size;

	dir = pgd_offset(&init_task, address);
	while (address < end) {
		pmd_t *pmd = pmd_alloc_kernel(dir, address);
		if (!pmd)
			return -ENOMEM;
		if (alloc_area_pmd(pmd, address, end - address))
			return -ENOMEM;
		set_pgdir(address, *dir);
		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		dir++;
	}
	invalidate();
	return 0;
}

void vfree(void * addr)
{
	struct vm_struct **p, *tmp;

	if (!addr)
		return;
	if ((PAGE_SIZE-1) & (unsigned long) addr) {
		printk("Trying to vfree() bad address (%p)\n", addr);
		return;
	}
	for (p = &vmlist ; (tmp = *p) ; p = &tmp->next) {
		if (tmp->addr == addr) {
			*p = tmp->next;
			free_area_pages(VMALLOC_VMADDR(tmp->addr), tmp->size);
			kfree(tmp);
			return;
		}
	}
	printk("Trying to vfree() nonexistent vm area (%p)\n", addr);
}

void * vmalloc(unsigned long size)
{
	void * addr;
	struct vm_struct **p, *tmp, *area;

	size = PAGE_ALIGN(size);
	if (!size || size > high_memory)
		return NULL;
	area = (struct vm_struct *) kmalloc(sizeof(*area), GFP_KERNEL);
	if (!area)
		return NULL;
	addr = (void *) VMALLOC_START;
	area->size = size + PAGE_SIZE;
	area->next = NULL;
	for (p = &vmlist; (tmp = *p) ; p = &tmp->next) {
		if (size + (unsigned long) addr < (unsigned long) tmp->addr)
			break;
		addr = (void *) (tmp->size + (unsigned long) tmp->addr);
	}
	area->addr = addr;
	area->next = *p;
	*p = area;
	if (alloc_area_pages(VMALLOC_VMADDR(addr), size)) {
		vfree(addr);
		return NULL;
	}
	return addr;
}

int vread(char *buf, char *addr, int count)
{
	struct vm_struct **p, *tmp;
	char *vaddr, *buf_start = buf;
	int n;

	for (p = &vmlist; (tmp = *p) ; p = &tmp->next) {
		vaddr = (char *) tmp->addr;
		while (addr < vaddr) {
			if (count == 0)
				goto finished;
			put_fs_byte('\0', buf++), addr++, count--;
		}
		n = tmp->size - PAGE_SIZE;
		if (addr > vaddr)
			n -= addr - vaddr;
		while (--n >= 0) {
			if (count == 0)
				goto finished;
			put_fs_byte(*addr++, buf++), count--;
		}
	}
finished:
	return buf - buf_start;
}
