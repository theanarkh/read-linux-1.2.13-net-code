/*
 *  linux/fs/minix/symlink.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  minix symlink handling code
 */

#ifdef MODULE
#include <linux/module.h>
#endif

#include <asm/segment.h>

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/minix_fs.h>
#include <linux/stat.h>

static int minix_readlink(struct inode *, char *, int);
static int minix_follow_link(struct inode *, struct inode *, int, int, struct inode **);

/*
 * symlinks can't do much...
 */
// 操作软链接文件的函数集，在新建软链接文件的时候赋值给inode结构体
struct inode_operations minix_symlink_inode_operations = {
	NULL,			/* no file-operations */
	NULL,			/* create */
	NULL,			/* lookup */
	NULL,			/* link */
	NULL,			/* unlink */
	NULL,			/* symlink */
	NULL,			/* mkdir */
	NULL,			/* rmdir */
	NULL,			/* mknod */
	NULL,			/* rename */
	minix_readlink,		/* readlink */
	minix_follow_link,	/* follow_link */
	NULL,			/* bmap */
	NULL,			/* truncate */
	NULL			/* permission */
};
// 打开软链对应的文件
static int minix_follow_link(struct inode * dir, struct inode * inode,
	int flag, int mode, struct inode ** res_inode)
{
	int error;
	struct buffer_head * bh;

	*res_inode = NULL;
	if (!dir) {
		dir = current->fs->root;
		dir->i_count++;
	}
	if (!inode) {
		iput(dir);
		return -ENOENT;
	}
	if (!S_ISLNK(inode->i_mode)) {
		iput(dir);
		*res_inode = inode;
		return 0;
	}
	if (current->link_count > 5) {
		iput(inode);
		iput(dir);
		return -ELOOP;
	}
	// 读取文件第一块内容
	if (!(bh = minix_bread(inode, 0, 0))) {
		iput(inode);
		iput(dir);
		return -EIO;
	}
	iput(inode);
	current->link_count++;
	// 打开b_data里的保存的文件名对应的文件
	error = open_namei(bh->b_data,flag,mode,res_inode,dir);
	current->link_count--;
	brelse(bh);
	return error;
}
// 读取软链文件的内容，即文件路径
static int minix_readlink(struct inode * inode, char * buffer, int buflen)
{
	struct buffer_head * bh;
	int i;
	char c;

	if (!S_ISLNK(inode->i_mode)) {
		iput(inode);
		return -EINVAL;
	}
	if (buflen > 1023)
		buflen = 1023;
	bh = minix_bread(inode, 0, 0);
	iput(inode);
	if (!bh)
		return 0;
	i = 0;
	while (i<buflen && (c = bh->b_data[i])) {
		i++;
		put_fs_byte(c,buffer++);
	}
	brelse(bh);
	return i;
}
