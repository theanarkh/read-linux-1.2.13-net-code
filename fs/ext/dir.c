/*
 *  linux/fs/ext/dir.c
 *
 *  Copyright (C) 1992 Remy Card (card@masi.ibp.fr)
 *
 *  from
 *
 *  linux/fs/minix/dir.c
 *
 *  Copyright (C) 1991, 1992 Linus Torvalds
 *
 *  ext directory handling functions
 */

#include <asm/segment.h>

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/ext_fs.h>
#include <linux/stat.h>

#define NAME_OFFSET(de) ((int) ((de)->d_name - (char *) (de)))
#define ROUND_UP(x) (((x)+3) & ~3)

static int ext_dir_read(struct inode * inode, struct file * filp, char * buf, int count)
{
	return -EISDIR;
}

static int ext_readdir(struct inode *, struct file *, struct dirent *, int);

static struct file_operations ext_dir_operations = {
	NULL,			/* lseek - default */
	ext_dir_read,		/* read */
	NULL,			/* write - bad */
	ext_readdir,		/* readdir */
	NULL,			/* select - default */
	NULL,			/* ioctl - default */
	NULL,			/* mmap */
	NULL,			/* no special open code */
	NULL,			/* no special release code */
	file_fsync		/* fsync */
};

/*
 * directories can handle most operations...
 */
struct inode_operations ext_dir_inode_operations = {
	&ext_dir_operations,	/* default directory file-ops */
	ext_create,		/* create */
	ext_lookup,		/* lookup */
	ext_link,		/* link */
	ext_unlink,		/* unlink */
	ext_symlink,		/* symlink */
	ext_mkdir,		/* mkdir */
	ext_rmdir,		/* rmdir */
	ext_mknod,		/* mknod */
	ext_rename,		/* rename */
	NULL,			/* readlink */
	NULL,			/* follow_link */
	NULL,			/* bmap */
	ext_truncate,		/* truncate */
	NULL			/* permission */
};
// 把inode对应的目录下所有文件、目录读出来，filp是inode对应的file结构体，count是读取的个数
static int ext_readdir(struct inode * inode, struct file * filp,
	struct dirent * dirent, int count)
{
	unsigned int i;
	unsigned int ret;
	off_t offset;
	char c;
	struct buffer_head * bh;
	struct ext_dir_entry * de;

	if (!inode || !S_ISDIR(inode->i_mode))
		return -EBADF;
	if ((filp->f_pos & 7) != 0)
		return -EBADF;
	ret = 0;
	// 当前位置小于文件大小，即还没读完，处理每块中的目录项
	while (!ret && filp->f_pos < inode->i_size) {
		// 从第n块数据，偏移为offset的地址开始读
		offset = filp->f_pos & 1023;
		// 读取inode对应的文件内容的第几块数据，0表示只读，不创建新的块
		bh = ext_bread(inode,(filp->f_pos)>>BLOCK_SIZE_BITS,0);
		// 读取失败，跳过这个块，1024是一个块的大小，减去offset即准备要读的大小，跳过他
		if (!bh) {
			filp->f_pos += 1024-offset;
			continue;
		}
		// 找到大于offset的第一个entry首地址或者rec_len为0的entry首地址
		for (i = 0; i < 1024 && i < offset; ) {
			de = (struct ext_dir_entry *) (bh->b_data + i);
			if (!de->rec_len)
				break;
			i += de->rec_len;
		}
		offset = i;
		// 当前待处理的目录项
		de = (struct ext_dir_entry *) (offset + bh->b_data);
		// 处理一块中的目录项
		while (!ret && offset < 1024 && filp->f_pos < inode->i_size) {
			if (de->rec_len < 8 || de->rec_len % 8 != 0 ||
			    de->rec_len < de->name_len + 8 ||
			    (de->rec_len + (off_t) filp->f_pos - 1) / 1024 > ((off_t) filp->f_pos / 1024)) {
				printk ("ext_readdir: bad dir entry, skipping\n");
				printk ("dev=%d, dir=%ld, offset=%ld, rec_len=%d, name_len=%d\n",
					inode->i_dev, inode->i_ino, offset, de->rec_len, de->name_len);
				filp->f_pos += 1024-offset;
				if (filp->f_pos > inode->i_size)
					filp->f_pos = inode->i_size;
				continue;
			}
			// 下一个entry的位置
			offset += de->rec_len;
			filp->f_pos += de->rec_len;
			// 有效的节点则处理
			if (de->inode) {
				// 复制名字到dirent
				for (i = 0; i < de->name_len; i++)
					if ((c = de->name[i]) != 0)
						put_fs_byte(c,i+dirent->d_name);
					else
						break;
				// 名字不为空则不为0
				if (i) {
					put_fs_long(de->inode,&dirent->d_ino);
					// 结尾字符
					put_fs_byte(0,i+dirent->d_name);
					put_fs_word(i,&dirent->d_reclen);
					ret = ROUND_UP(NAME_OFFSET(dirent)+i+1);
					break;
				}
			}
			de = (struct ext_dir_entry *) ((char *) de 
				+ de->rec_len);
		}
		brelse(bh);
	}
	return ret;
}
