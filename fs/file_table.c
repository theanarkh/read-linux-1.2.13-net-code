/*
 *  linux/fs/file_table.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/fs.h>
#include <linux/string.h>
#include <linux/mm.h>

struct file * first_file;
int nr_files = 0;

// 双向循环链表，first_file指向头指针，头插法插入一个节点
static void insert_file_free(struct file *file)
{
	file->f_next = first_file;
	file->f_prev = first_file->f_prev;
	file->f_next->f_prev = file;
	file->f_prev->f_next = file;
	first_file = file;
}
// 删除一个节点
static void remove_file_free(struct file *file)
{	
	// 如果要删除的节点是第一个节点，则更新头指针，指向下一个节点
	if (first_file == file)
		first_file = first_file->f_next;
	// 如果被删除的节点后面还有节点，则需要更新下一个节点的prev指针，指向当前节点的上一个节点
	if (file->f_next)
		file->f_next->f_prev = file->f_prev;
	// 同理，更新上一个节点的next指针，指向被删除节点的下一个节点
	if (file->f_prev)
		file->f_prev->f_next = file->f_next;
	// 置空
	file->f_next = file->f_prev = NULL;
}

// file插入链表，成为最后一个节点
static void put_last_free(struct file *file)
{	
	// 保证file脱离了原来的链表
	remove_file_free(file);
	// 插入链表，但是不更新头指针first_file，所以file成为最后一个节点
	file->f_prev = first_file->f_prev;
	file->f_prev->f_next = file;
	file->f_next = first_file;
	file->f_next->f_prev = file;
}

void grow_files(void)
{
	struct file * file;
	int i;
	// 申请一页内存
	file = (struct file *) get_free_page(GFP_KERNEL);

	if (!file)
		return;
	// i=PAGE_SIZE/sizeof(struct file),即一页可以存多少个节点，更新最大节点数
	nr_files+=i= PAGE_SIZE/sizeof(struct file);
	/*
	 当前是初始化的时候，先初始化一个节点，需要初始化的节点数减一,执行insert_file_free
	 前需要保证first_file非空，见insert_file_free中的first_file
	*/
	if (!first_file)
		file->f_next = file->f_prev = first_file = file++, i--;
	// 形成一个链表
	for (; i ; i--)
		insert_file_free(file++);
}
// file链表初始化
unsigned long file_table_init(unsigned long start, unsigned long end)
{
	first_file = NULL;
	return start;
}

// 获取一个可以的file结构体
struct file * get_empty_filp(void)
{
	int i;
	struct file * f;

	if (!first_file)
		grow_files();
repeat:
	// nr_files是链表的总节点数
	for (f = first_file, i=0; i < nr_files; i++, f = f->f_next)
		// 找到空闲的节点
		if (!f->f_count) {
			// 脱离链表
			remove_file_free(f);
			// 清空内存
			memset(f,0,sizeof(*f));
			// 插入链表末尾
			put_last_free(f);
			// 标记已使用
			f->f_count = 1;
			f->f_version = ++event;
			return f;
		}
	// 没有找到空闲节点，扩容，再找
	if (nr_files < NR_FILE) {
		grow_files();
		goto repeat;
	}
	return NULL;
}
