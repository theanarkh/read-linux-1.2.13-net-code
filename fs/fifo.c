/*
 *  linux/fs/fifo.c
 *
 *  written by Paul H. Hargrove
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/mm.h>

static int fifo_open(struct inode * inode,struct file * filp)
{
	int retval = 0;
	unsigned long page;

	switch( filp->f_mode ) {

	case 1:
	/*
	 *  O_RDONLY
	 *  POSIX.1 says that O_NONBLOCK means return with the FIFO
	 *  opened, even when there is no process writing the FIFO.
	 */
		filp->f_op = &connecting_fifo_fops;
		// if为true说明之前没有读者，现在新增了一个，唤醒等待写的进程
		if (!PIPE_READERS(*inode)++)
			wake_up_interruptible(&PIPE_WAIT(*inode));
		// 如果没有设置非阻塞标记或者现在有写者，会导致当前进程阻塞
		if (!(filp->f_flags & O_NONBLOCK) && !PIPE_WRITERS(*inode)) {
			// 等待打开该命名管道的读者打开数加一
			PIPE_RD_OPENERS(*inode)++;
			// 现在没有写者，则先阻塞读者进程
			while (!PIPE_WRITERS(*inode)) {
				// 判断是不是因为被信号唤醒的，而不是因为有写者了
				if (current->signal & ~current->blocked) {
					retval = -ERESTARTSYS;
					break;
				}
				// 当前进程阻塞在这个inode上，可被信号唤醒
				interruptible_sleep_on(&PIPE_WAIT(*inode));
			}
			// 有写者了，如果有进程在等待读者，则唤醒他
			if (!--PIPE_RD_OPENERS(*inode))
				wake_up_interruptible(&PIPE_WAIT(*inode));
		}
		// 有等待写打开的进程，阻塞，等待写打开后被唤醒
		while (PIPE_WR_OPENERS(*inode))
			interruptible_sleep_on(&PIPE_WAIT(*inode));
		// 有写者
		if (PIPE_WRITERS(*inode))
			filp->f_op = &read_fifo_fops;
		if (retval && !--PIPE_READERS(*inode))
			wake_up_interruptible(&PIPE_WAIT(*inode));
		break;
	
	case 2:
	/*
	 *  O_WRONLY
	 *  POSIX.1 says that O_NONBLOCK means return -1 with
	 *  errno=ENXIO when there is no process reading the FIFO.
	 */
		// 设置了非阻塞并且没有读者，则直接返回
		if ((filp->f_flags & O_NONBLOCK) && !PIPE_READERS(*inode)) {
			retval = -ENXIO;
			break;
		}
		// 没有设置非阻塞标记 或者 有读者
		// 设置函数集
		filp->f_op = &write_fifo_fops;
		// 写者加一，如果之前没有写者，则唤醒因没有写者而阻塞的进程（比如第一次打开读的时候，因为没有写进程而阻塞）
		if (!PIPE_WRITERS(*inode)++)
			wake_up_interruptible(&PIPE_WAIT(*inode));
		// 没有读者
		if (!PIPE_READERS(*inode)) {
			// 等待打开写的进程数加一，等待有读者
			PIPE_WR_OPENERS(*inode)++;
			// 没有读者，不能打开写，阻塞当前进程
			while (!PIPE_READERS(*inode)) {
				if (current->signal & ~current->blocked) {
					retval = -ERESTARTSYS;	
					break;
				}
				interruptible_sleep_on(&PIPE_WAIT(*inode));
			}
			// 说明有读者了，如果没有等待打开写的进程，则唤醒因有等待打开写而阻塞的进程
			if (!--PIPE_WR_OPENERS(*inode))
				wake_up_interruptible(&PIPE_WAIT(*inode));
		}
		// 有读者或者被信号唤醒了，有读者但是因为没有写进程而阻塞了，还不算有读者，继续阻塞
		while (PIPE_RD_OPENERS(*inode))
			interruptible_sleep_on(&PIPE_WAIT(*inode));
		// 被信号唤醒了，读者数减一，和上面的++对称。如果减一后写者为0，则唤醒因有写者而阻塞的进程
		if (retval && !--PIPE_WRITERS(*inode))
			wake_up_interruptible(&PIPE_WAIT(*inode));
		break;
	
	case 3:
	/*
	 *  O_RDWR
	 *  POSIX.1 leaves this case "undefined" when O_NONBLOCK is set.
	 *  This implementation will NEVER block on a O_RDWR open, since
	 *  the process can at least talk to itself.
	 */
		filp->f_op = &rdwr_fifo_fops;
		// 此时没有读者，则唤醒等到读者的进程（如果有的话），因为这里以读写方式打开管道，当前进程就是一个读者
		if (!PIPE_READERS(*inode)++)
			wake_up_interruptible(&PIPE_WAIT(*inode));
		// 有等待写的进程，
		while (PIPE_WR_OPENERS(*inode))
			interruptible_sleep_on(&PIPE_WAIT(*inode));
		// 此时没有写者，同上
		if (!PIPE_WRITERS(*inode)++)
			wake_up_interruptible(&PIPE_WAIT(*inode));

		while (PIPE_RD_OPENERS(*inode))
			interruptible_sleep_on(&PIPE_WAIT(*inode));
		break;

	default:
		retval = -EINVAL;
	}
	if (retval || PIPE_BASE(*inode))
		return retval;
	// 获取一页内存用于管道通信
	page = __get_free_page(GFP_KERNEL);
	if (PIPE_BASE(*inode)) {
		free_page(page);
		return 0;
	}
	if (!page)
		return -ENOMEM;
	PIPE_LOCK(*inode) = 0;
	// 初始化字段
	PIPE_START(*inode) = PIPE_LEN(*inode) = 0;
	// 执行使用的内存首地址
	PIPE_BASE(*inode) = (char *) page;
	return 0;
}

/*
 * Dummy default file-operations: the only thing this does
 * is contain the open that then fills in the correct operations
 * depending on the access mode of the file...
 */
static struct file_operations def_fifo_fops = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	fifo_open,		/* will set read or write pipe_fops */
	NULL,
	NULL
};

static struct inode_operations fifo_inode_operations = {
	&def_fifo_fops,		/* default file operations */
	NULL,			/* create */
	NULL,			/* lookup */
	NULL,			/* link */
	NULL,			/* unlink */
	NULL,			/* symlink */
	NULL,			/* mkdir */
	NULL,			/* rmdir */
	NULL,			/* mknod */
	NULL,			/* rename */
	NULL,			/* readlink */
	NULL,			/* follow_link */
	NULL,			/* bmap */
	NULL,			/* truncate */
	NULL			/* permission */
};

void init_fifo(struct inode * inode)
{
	// 操作函数集
	inode->i_op = &fifo_inode_operations;
	// 是管道
	inode->i_pipe = 1;
	// 初始化各字段
	PIPE_LOCK(*inode) = 0;
	PIPE_BASE(*inode) = NULL;
	PIPE_START(*inode) = PIPE_LEN(*inode) = 0;
	PIPE_RD_OPENERS(*inode) = PIPE_WR_OPENERS(*inode) = 0;
	PIPE_WAIT(*inode) = NULL;
	PIPE_READERS(*inode) = PIPE_WRITERS(*inode) = 0;
}
