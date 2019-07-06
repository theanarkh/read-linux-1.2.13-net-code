/*
 *  linux/fs/inode.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/stat.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>

#include <asm/system.h>

static struct inode_hash_entry {
	struct inode * inode;
	int updating;
} hash_table[NR_IHASH];

static struct inode * first_inode;
static struct wait_queue * inode_wait = NULL;
static int nr_inodes = 0, nr_free_inodes = 0;
// 哈希函数
static inline int const hashfn(dev_t dev, unsigned int i)
{
	return (dev ^ i) % NR_IHASH;
}
// 算出在哈希表的地址
static inline struct inode_hash_entry * const hash(dev_t dev, int i)
{
	return hash_table + hashfn(dev, i);
}
// 头插法插入inode
static void insert_inode_free(struct inode *inode)
{
	inode->i_next = first_inode;
	inode->i_prev = first_inode->i_prev;
	inode->i_next->i_prev = inode;
	inode->i_prev->i_next = inode;
	first_inode = inode;
}

// inode脱离链表，更新first_inode指针
static void remove_inode_free(struct inode *inode)
{
	if (first_inode == inode)
		first_inode = first_inode->i_next;
	if (inode->i_next)
		inode->i_next->i_prev = inode->i_prev;
	if (inode->i_prev)
		inode->i_prev->i_next = inode->i_next;
	inode->i_next = inode->i_prev = NULL;
}

void insert_inode_hash(struct inode *inode)
{
	struct inode_hash_entry *h;
	// 得到插入的位置
	h = hash(inode->i_dev, inode->i_ino);
	// 头插法插入哈希链表
	inode->i_hash_next = h->inode;
	inode->i_hash_prev = NULL;
	// 更新下一个节点的pre指针。即插入之前，链表不为空
	if (inode->i_hash_next)
		inode->i_hash_next->i_hash_prev = inode;
	// 更新头指针
	h->inode = inode;
}

static void remove_inode_hash(struct inode *inode)
{
	struct inode_hash_entry *h;
	// 算出inode在哈希表的位置
	h = hash(inode->i_dev, inode->i_ino);
	// 参考上面的删除函数
	if (h->inode == inode)
		h->inode = inode->i_hash_next;
	if (inode->i_hash_next)
		inode->i_hash_next->i_hash_prev = inode->i_hash_prev;
	if (inode->i_hash_prev)
		inode->i_hash_prev->i_hash_next = inode->i_hash_next;
	inode->i_hash_prev = inode->i_hash_next = NULL;
}
// 把inode插入到链表的末尾
static void put_last_free(struct inode *inode)
{
	remove_inode_free(inode);
	inode->i_prev = first_inode->i_prev;
	inode->i_prev->i_next = inode;
	inode->i_next = first_inode;
	inode->i_next->i_prev = inode;
}
// 扩容
void grow_inodes(void)
{
	struct inode * inode;
	int i;

	if (!(inode = (struct inode*) get_free_page(GFP_KERNEL)))
		return;
	// 一页包括的inode结构体数
	i=PAGE_SIZE / sizeof(struct inode);
	// 总数累加
	nr_inodes += i;
	// 空闲数累加
	nr_free_inodes += i;
	// 参考insert_inode_free代码，保证first_inode非空 
	if (!first_inode)
		inode->i_next = inode->i_prev = first_inode = inode++, i--;
	// 插入链表
	for ( ; i ; i-- )
		insert_inode_free(inode++);
}
// 初始化哈希表和空闲节点链表的头指针
unsigned long inode_init(unsigned long start, unsigned long end)
{
	memset(hash_table, 0, sizeof(hash_table));
	first_inode = NULL;
	return start;
}

static void __wait_on_inode(struct inode *);

static inline void wait_on_inode(struct inode * inode)
{	
	// 如果已经被锁，则阻塞，插入inode的等待队列中
	if (inode->i_lock)
		__wait_on_inode(inode);
}
// 加锁
static inline void lock_inode(struct inode * inode)
{
	wait_on_inode(inode);
	inode->i_lock = 1;
}
// 解锁，唤醒等待队列
static inline void unlock_inode(struct inode * inode)
{
	inode->i_lock = 0;
	wake_up(&inode->i_wait);
}

/*
 * Note that we don't want to disturb any wait-queues when we discard
 * an inode.
 *
 * Argghh. Got bitten by a gcc problem with inlining: no way to tell
 * the compiler that the inline asm function 'memset' changes 'inode'.
 * I've been searching for the bug for days, and was getting desperate.
 * Finally looked at the assembler output... Grrr.
 *
 * The solution is the weird use of 'volatile'. Ho humm. Have to report
 * it to the gcc lists, and hope we can do this more cleanly some day..
 */
void clear_inode(struct inode * inode)
{
	struct wait_queue * wait;
	// 如果被锁了，等待inode释放
	wait_on_inode(inode);
	// 从哈希链表中删除该节点
	remove_inode_hash(inode);
	// 从空闲链表中删除该节点
	remove_inode_free(inode);
	// 阻塞在该inode进程队列
	wait = ((volatile struct inode *) inode)->i_wait;
	if (inode->i_count)
		nr_free_inodes++;
	// 清空inode内容
	memset(inode,0,sizeof(*inode));
	((volatile struct inode *) inode)->i_wait = wait;
	// 作为空闲项插入链表
	insert_inode_free(inode);
}
// 释放dev设备在first_inode链表的节点
int fs_may_mount(dev_t dev)
{
	struct inode * inode, * next;
	int i;

	next = first_inode;
	for (i = nr_inodes ; i > 0 ; i--) {
		inode = next;
		next = inode->i_next;	/* clear_inode() changes the queues.. */
		if (inode->i_dev != dev)
			continue;
		// 有进程引用或者数据还没同步到硬盘或者被锁了则返回
		if (inode->i_count || inode->i_dirt || inode->i_lock)
			return 0;
		// 释放inode
		clear_inode(inode);
	}
	return 1;
}

int fs_may_umount(dev_t dev, struct inode * mount_root)
{
	struct inode * inode;
	int i;

	inode = first_inode;
	for (i=0 ; i < nr_inodes ; i++, inode = inode->i_next) {
		if (inode->i_dev != dev || !inode->i_count)
			continue;
		if (inode == mount_root && inode->i_count == 1)
			continue;
		return 0;
	}
	return 1;
}

int fs_may_remount_ro(dev_t dev)
{
	struct file * file;
	int i;

	/* Check that no files are currently opened for writing. */
	for (file = first_file, i=0; i<nr_files; i++, file=file->f_next) {
		if (!file->f_count || !file->f_inode ||
		    file->f_inode->i_dev != dev)
			continue;
		if (S_ISREG(file->f_inode->i_mode) && (file->f_mode & 2))
			return 0;
	}
	return 1;
}

static void write_inode(struct inode * inode)
{
	if (!inode->i_dirt)
		return;
	wait_on_inode(inode);
	if (!inode->i_dirt)
		return;
	if (!inode->i_sb || !inode->i_sb->s_op || !inode->i_sb->s_op->write_inode) {
		inode->i_dirt = 0;
		return;
	}
	inode->i_lock = 1;	
	inode->i_sb->s_op->write_inode(inode);
	unlock_inode(inode);
}

static void read_inode(struct inode * inode)
{
	lock_inode(inode);
	if (inode->i_sb && inode->i_sb->s_op && inode->i_sb->s_op->read_inode)
		inode->i_sb->s_op->read_inode(inode);
	unlock_inode(inode);
}

/* POSIX UID/GID verification for setting inode attributes */
int inode_change_ok(struct inode *inode, struct iattr *attr)
{
	/* Make sure a caller can chown */
	if ((attr->ia_valid & ATTR_UID) &&
	    (current->fsuid != inode->i_uid ||
	     attr->ia_uid != inode->i_uid) && !fsuser())
		return -EPERM;

	/* Make sure caller can chgrp */
	if ((attr->ia_valid & ATTR_GID) &&
	    (!in_group_p(attr->ia_gid) && attr->ia_gid != inode->i_gid) &&
	    !fsuser())
		return -EPERM;

	/* Make sure a caller can chmod */
	if (attr->ia_valid & ATTR_MODE) {
		if ((current->fsuid != inode->i_uid) && !fsuser())
			return -EPERM;
		/* Also check the setgid bit! */
		if (!fsuser() && !in_group_p((attr->ia_valid & ATTR_GID) ? attr->ia_gid :
					     inode->i_gid))
			attr->ia_mode &= ~S_ISGID;
	}

	/* Check for setting the inode time */
	if ((attr->ia_valid & ATTR_ATIME_SET) &&
	    ((current->fsuid != inode->i_uid) && !fsuser()))
		return -EPERM;
	if ((attr->ia_valid & ATTR_MTIME_SET) &&
	    ((current->fsuid != inode->i_uid) && !fsuser()))
		return -EPERM;


	return 0;
}

/*
 * Set the appropriate attributes from an attribute structure into
 * the inode structure.
 */
// 设置inode的属性
void inode_setattr(struct inode *inode, struct iattr *attr)
{
	if (attr->ia_valid & ATTR_UID)
		inode->i_uid = attr->ia_uid;
	if (attr->ia_valid & ATTR_GID)
		inode->i_gid = attr->ia_gid;
	if (attr->ia_valid & ATTR_SIZE)
		inode->i_size = attr->ia_size;
	if (attr->ia_valid & ATTR_ATIME)
		inode->i_atime = attr->ia_atime;
	if (attr->ia_valid & ATTR_MTIME)
		inode->i_mtime = attr->ia_mtime;
	if (attr->ia_valid & ATTR_CTIME)
		inode->i_ctime = attr->ia_ctime;
	if (attr->ia_valid & ATTR_MODE) {
		inode->i_mode = attr->ia_mode;
		if (!fsuser() && !in_group_p(inode->i_gid))
			inode->i_mode &= ~S_ISGID;
	}
	inode->i_dirt = 1;
}

/*
 * notify_change is called for inode-changing operations such as
 * chown, chmod, utime, and truncate.  It is guaranteed (unlike
 * write_inode) to be called from the context of the user requesting
 * the change.  It is not called for ordinary access-time updates.
 * NFS uses this to get the authentication correct.  -- jrs
 */

int notify_change(struct inode * inode, struct iattr *attr)
{
	int retval;

	if (inode->i_sb && inode->i_sb->s_op  &&
	    inode->i_sb->s_op->notify_change) 
		return inode->i_sb->s_op->notify_change(inode, attr);

	if ((retval = inode_change_ok(inode, attr)) != 0)
		return retval;

	inode_setattr(inode, attr);
	return 0;
}

/*
 * bmap is needed for demand-loading and paging: if this function
 * doesn't exist for a filesystem, then those things are impossible:
 * executables cannot be run from the filesystem etc...
 *
 * This isn't as bad as it sounds: the read-routines might still work,
 * so the filesystem would be otherwise ok (for example, you might have
 * a DOS filesystem, which doesn't lend itself to bmap very well, but
 * you could still transfer files to/from the filesystem)
 */
int bmap(struct inode * inode, int block)
{
	if (inode->i_op && inode->i_op->bmap)
		return inode->i_op->bmap(inode,block);
	return 0;
}

void invalidate_inodes(dev_t dev)
{
	struct inode * inode, * next;
	int i;

	next = first_inode;
	for(i = nr_inodes ; i > 0 ; i--) {
		inode = next;
		next = inode->i_next;		/* clear_inode() changes the queues.. */
		if (inode->i_dev != dev)
			continue;
		if (inode->i_count || inode->i_dirt || inode->i_lock) {
			printk("VFS: inode busy on removed device %d/%d\n", MAJOR(dev), MINOR(dev));
			continue;
		}
		clear_inode(inode);
	}
}
// 把first_inode链表中dev对应的inode回写到底层
void sync_inodes(dev_t dev)
{
	int i;
	struct inode * inode;

	inode = first_inode;
	for(i = 0; i < nr_inodes*2; i++, inode = inode->i_next) {
		if (dev && inode->i_dev != dev)
			continue;
		wait_on_inode(inode);
		if (inode->i_dirt)
			write_inode(inode);
	}
}

void iput(struct inode * inode)
{
	if (!inode)
		return;
	wait_on_inode(inode);
	// 没有进程使用，不需要回写
	if (!inode->i_count) {
		printk("VFS: iput: trying to free free inode\n");
		printk("VFS: device %d/%d, inode %lu, mode=0%07o\n",
			MAJOR(inode->i_rdev), MINOR(inode->i_rdev),
					inode->i_ino, inode->i_mode);
		return;
	}
	// 是管道
	if (inode->i_pipe)
		wake_up_interruptible(&PIPE_WAIT(*inode));
repeat:
	// 还有进程在使用，引用数减一后返回
	if (inode->i_count>1) {
		inode->i_count--;
		return;
	}
	// 唤醒等待队列的进程
	wake_up(&inode_wait);
	// 是管道文件则释放对应的一页内存，用来通信的
	if (inode->i_pipe) {
		unsigned long page = (unsigned long) PIPE_BASE(*inode);
		PIPE_BASE(*inode) = NULL;
		free_page(page);
	}
	if (inode->i_sb && inode->i_sb->s_op && inode->i_sb->s_op->put_inode) {
		inode->i_sb->s_op->put_inode(inode);
		if (!inode->i_nlink)
			return;
	}
	// 需要回写
	if (inode->i_dirt) {
		write_inode(inode);	/* we can sleep - so do again */
		wait_on_inode(inode);
		goto repeat;
	}
	// 引用数减一后为0
	inode->i_count--;
	if (inode->i_mmap) {
		printk("iput: inode %lu on device %d/%d still has mappings.\n",
			inode->i_ino, MAJOR(inode->i_dev), MINOR(inode->i_dev));
		inode->i_mmap = NULL;
	}
	// 空闲节点数加一
	nr_free_inodes++;
	return;
}
// 获取一个空闲的inode节点
struct inode * get_empty_inode(void)
{
	struct inode * inode, * best;
	int i;
	// 还没有达到节点数阈值并且空闲节点数小于四分之一，则扩容
	if (nr_inodes < NR_INODE && nr_free_inodes < (nr_inodes >> 2))
		grow_inodes();
repeat:
	inode = first_inode;
	best = NULL;
	for (i = 0; i<nr_inodes; inode = inode->i_next, i++) {
		// 该节点没有进程使用
		if (!inode->i_count) {
			// 还没有找到空闲的，则认为没有被进程引用的inode为最合适的节点，如果之前已经找到则不更新best了
			if (!best)
				best = inode;
			// 数据是最新的并且没有被锁则为最合适的节点
			if (!inode->i_dirt && !inode->i_lock) {
				best = inode;
				break;
			}
		}
	}
	// 没有找到合适的，或者找到了但是数据是脏的或者被锁了则扩容，函数一开始可能没有扩容，查找失败后再扩容
	if (!best || best->i_dirt || best->i_lock)
		if (nr_inodes < NR_INODE) {
			grow_inodes();
			// 重新找
			goto repeat;
		}
	
	inode = best;
	// 节点数达到阈值还没有找到，则是在等待inode的队列，唤醒后在重新找
	if (!inode) {
		printk("VFS: No free inodes - contact Linus\n");
		sleep_on(&inode_wait);
		goto repeat;
	}
	// 找到的时候没有被锁，进程切换的时候可能被其他进程锁了，则继续找
	if (inode->i_lock) {
		wait_on_inode(inode);
		goto repeat;
	}
	// 同上
	if (inode->i_dirt) {
		write_inode(inode);
		goto repeat;
	}
	// 同上
	if (inode->i_count)
		goto repeat;
	// 初始化inode
	clear_inode(inode);
	// 引用数是1
	inode->i_count = 1;
	inode->i_nlink = 1;
	inode->i_version = ++event;
	inode->i_sem.count = 1;
	// 空闲节点数减一
	nr_free_inodes--;
	if (nr_free_inodes < 0) {
		printk ("VFS: get_empty_inode: bad free inode count.\n");
		nr_free_inodes = 0;
	}
	return inode;
}
// 获取一个用于管道的inode
struct inode * get_pipe_inode(void)
{
	struct inode * inode;
	extern struct inode_operations pipe_inode_operations;
	// 获取一个inode
	if (!(inode = get_empty_inode()))
		return NULL;
	// 指向一页内存
	if (!(PIPE_BASE(*inode) = (char*) __get_free_page(GFP_USER))) {
		iput(inode);
		return NULL;
	}
	// 赋值操作函数集
	inode->i_op = &pipe_inode_operations;
	// 读写端，两个引用
	inode->i_count = 2;	/* sum of readers/writers */
	// 初始化字段
	PIPE_WAIT(*inode) = NULL;
	PIPE_START(*inode) = PIPE_LEN(*inode) = 0;
	PIPE_RD_OPENERS(*inode) = PIPE_WR_OPENERS(*inode) = 0;
	PIPE_READERS(*inode) = PIPE_WRITERS(*inode) = 1;
	PIPE_LOCK(*inode) = 0;
	// inode对应的是管道文件
	inode->i_pipe = 1;
	inode->i_mode |= S_IFIFO | S_IRUSR | S_IWUSR;
	inode->i_uid = current->fsuid;
	inode->i_gid = current->fsgid;
	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
	// 大小
	inode->i_blksize = PAGE_SIZE;
	return inode;
}
// 获取inode节点
struct inode * __iget(struct super_block * sb, int nr, int crossmntp)
{
	static struct wait_queue * update_wait = NULL;
	struct inode_hash_entry * h;
	struct inode * inode;
	struct inode * empty = NULL;

	if (!sb)
		panic("VFS: iget with sb==NULL");
	// 根据设备号和inode号获取哈希表位置
	h = hash(sb->s_dev, nr);
repeat:
	for (inode = h->inode; inode ; inode = inode->i_hash_next)
		// 设备相等并且inode号相等
		if (inode->i_dev == sb->s_dev && inode->i_ino == nr)
			goto found_it;
	
	if (!empty) {
		h->updating++;
		// 获取一个空闲inode
		empty = get_empty_inode();
		if (!--h->updating)
			wake_up(&update_wait);
		if (empty)
			goto repeat;
		return (NULL);
	}
	inode = empty;
	inode->i_sb = sb;
	inode->i_dev = sb->s_dev;
	inode->i_ino = nr;
	inode->i_flags = sb->s_flags;
	put_last_free(inode);
	insert_inode_hash(inode);
	read_inode(inode);
	goto return_it;

found_it:
	// 找到了，该inode还没有被引用则引用数减一，如果被引用了说明之前就减过一了
	if (!inode->i_count)
		nr_free_inodes--;
	// inode引用数加一
	inode->i_count++;
	// 可能被锁，需要阻塞
	wait_on_inode(inode);
	// 唤醒后发现被改了，重新找
	if (inode->i_dev != sb->s_dev || inode->i_ino != nr) {
		printk("Whee.. inode changed from under us. Tell Linus\n");
		iput(inode);
		goto repeat;
	}
	if (crossmntp && inode->i_mount) {
		struct inode * tmp = inode->i_mount;
		tmp->i_count++;
		iput(inode);
		inode = tmp;
		wait_on_inode(inode);
	}
	if (empty)
		iput(empty);

return_it:
	while (h->updating)
		sleep_on(&update_wait);
	return inode;
}

/*
 * The "new" scheduling primitives (new as of 0.97 or so) allow this to
 * be done without disabling interrupts (other than in the actual queue
 * updating things: only a couple of 386 instructions). This should be
 * much better for interrupt latency.
 */
static void __wait_on_inode(struct inode * inode)
{
	struct wait_queue wait = { current, NULL };
	/*
		把当前进程作为新节点挂载到inode->i_wait队列中,
		这里要先加入到等待队列，然后才能修改状态为阻塞状态。
		如果先修改状态，然后切换到其他进程执行了，就回不来了，
		因为加入了队列，等待其他进程的唤醒。
	*/
	add_wait_queue(&inode->i_wait, &wait);
repeat:
	// 阻塞
	current->state = TASK_UNINTERRUPTIBLE;
	// 如果被锁了，则调度其他进程执行，被唤醒后继续判断是否被锁了
	if (inode->i_lock) {
		schedule();
		goto repeat;
	}
	// 脱离等待队列
	remove_wait_queue(&inode->i_wait, &wait);
	// 修改进程状态
	current->state = TASK_RUNNING;
}
