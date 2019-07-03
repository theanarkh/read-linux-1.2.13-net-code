/*
 *  linux/fs/ext/symlink.c
 *
 *  Copyright (C) 1992 Remy Card (card@masi.ibp.fr)
 *
 *  from
 *
 *  linux/fs/minix/symlink.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext symlink handling code
 */

#include <asm/segment.h>

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/ext_fs.h>
#include <linux/stat.h>

static int ext_readlink(struct inode *, char *, int);
static int ext_follow_link(struct inode *, struct inode *, int, int, struct inode **);

/*
 * symlinks can't do much...
 */
struct inode_operations ext_symlink_inode_operations = {
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
	ext_readlink,		/* readlink */
	ext_follow_link,	/* follow_link */
	NULL,			/* bmap */
	NULL,			/* truncate */
	NULL			/* permission */
};
// 软链文件里的内容是一个文件路径，先读取文件路径，再查找文件路径对应的文件
static int ext_follow_link(struct inode * dir, struct inode * inode,
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
	// 进程同时打开的软链文件数不能超过5个
	if (current->link_count > 5) {
		iput(dir);
		iput(inode);
		return -ELOOP;
	}
	// 把软链文件的内容读进来
	if (!(bh = ext_bread(inode, 0, 0))) {
		iput(inode);
		iput(dir);
		return -EIO;
	}
	iput(inode);
	current->link_count++;
	// b_data就是文件路径，获取文件路径对应的文件的inode节点
	error = open_namei(bh->b_data,flag,mode,res_inode,dir);
	current->link_count--;
	brelse(bh);
	return error;
}

// 读取软链里的文件路径，写入buffer变量
static int ext_readlink(struct inode * inode, char * buffer, int buflen)
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
	bh = ext_bread(inode, 0, 0);
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
