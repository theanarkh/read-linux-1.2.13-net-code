/*
 *  linux/fs/minix/dir.c
 *
 *  Copyright (C) 1991, 1992 Linus Torvalds
 *
 *  minix directory handling functions
 */

#ifdef MODULE
#include <linux/module.h>
#endif

#include <asm/segment.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/minix_fs.h>
#include <linux/stat.h>

#define NAME_OFFSET(de) ((int) ((de)->d_name - (char *) (de)))
#define ROUND_UP(x) (((x)+3) & ~3)

static int minix_dir_read(struct inode * inode, struct file * filp, char * buf, int count)
{
	return -EISDIR;
}

static int minix_readdir(struct inode *, struct file *, struct dirent *, int);

static struct file_operations minix_dir_operations = {
	NULL,			/* lseek - default */
	minix_dir_read,		/* read */
	NULL,			/* write - bad */
	minix_readdir,		/* readdir */
	NULL,			/* select - default */
	NULL,			/* ioctl - default */
	NULL,			/* mmap */
	NULL,			/* no special open code */
	NULL,			/* no special release code */
	file_fsync		/* default fsync */
};

/*
 * directories can handle most operations...
 */
struct inode_operations minix_dir_inode_operations = {
	&minix_dir_operations,	/* default directory file-ops */
	minix_create,		/* create */
	minix_lookup,		/* lookup */
	minix_link,		/* link */
	minix_unlink,		/* unlink */
	minix_symlink,		/* symlink */
	minix_mkdir,		/* mkdir */
	minix_rmdir,		/* rmdir */
	minix_mknod,		/* mknod */
	minix_rename,		/* rename */
	NULL,			/* readlink */
	NULL,			/* follow_link */
	NULL,			/* bmap */
	minix_truncate,		/* truncate */
	NULL			/* permission */
};

static int minix_readdir(struct inode * inode, struct file * filp,
	struct dirent * dirent, int count)
{
	unsigned int offset,i,ret;
	int version;
	char c;
	struct buffer_head * bh;
	struct minix_dir_entry * de;
	struct minix_sb_info * info;
	
	if (!inode || !inode->i_sb || !S_ISDIR(inode->i_mode))
		return -EBADF;
	// minix超级块内容
	info = &inode->i_sb->u.minix_sb;
	// 要按目录项大小对齐
	if (filp->f_pos & (info->s_dirsize - 1))
		return -EBADF;
	ret = 0;
	// 还没找到并且小于文件大小
	while (!ret && filp->f_pos < inode->i_size) {
		// 取块内偏移
		offset = filp->f_pos & 1023;
		// 读取第n块内容,n=(filp->f_pos)>>BLOCK_SIZE_BITS
		bh = minix_bread(inode,(filp->f_pos)>>BLOCK_SIZE_BITS,0);
		// 读失败则跳过
		if (!bh) {
			filp->f_pos += 1024-offset;
			continue;
		}
		// 还没找到并且还没读完这一块内容，并且还没到文件末尾
		while (!ret && offset < 1024 && filp->f_pos < inode->i_size) {
			// 指向目录项首地址
			de = (struct minix_dir_entry *) (offset + bh->b_data);
			// 下一个目录项首地址，是块内偏移
			offset += info->s_dirsize;
			// 更新总偏移
			filp->f_pos += info->s_dirsize;
retry:
			// 有效的目录项，即inode号不为空
			if (de->inode) {
				version = inode->i_version;
				// 文件名长度的最大值,复制到dirent
				for (i = 0; i < info->s_namelen; i++)
					if ((c = de->name[i]) != 0)
						put_fs_byte(c,i+dirent->d_name);
					else
						break;
				// 文件名长度大于0，即有效
				if (i) {
					// 复制inode号等信息
					put_fs_long(de->inode,&dirent->d_ino);
					put_fs_byte(0,i+dirent->d_name);
					put_fs_word(i,&dirent->d_reclen);
					if (version != inode->i_version)
						goto retry;
					ret = ROUND_UP(NAME_OFFSET(dirent)+i+1);
				}
			}
		}
		brelse(bh);
	}
	return ret;
}
