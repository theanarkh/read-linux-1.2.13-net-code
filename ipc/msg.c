/*
 * linux/ipc/msg.c
 * Copyright (C) 1992 Krishna Balasubramanian 
 */

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/msg.h>
#include <linux/stat.h>
#include <linux/malloc.h>

#include <asm/segment.h>

extern int ipcperms (struct ipc_perm *ipcp, short msgflg);

static void freeque (int id);
static int newque (key_t key, int msgflg);
static int findkey (key_t key);

static struct msqid_ds *msgque[MSGMNI];
// 所有消息队列的字节数大小
static int msgbytes = 0;
static int msghdrs = 0;
static unsigned short msg_seq = 0;
// 系统使用的消息队列数
static int used_queues = 0;
// 当前消息队列的最大id
static int max_msqid = 0;
// 没有内存而阻塞的队列
static struct wait_queue *msg_lock = NULL;
// 系统启动的时候执行
void msg_init (void)
{
	int id;
	
	for (id = 0; id < MSGMNI; id++) 
		msgque[id] = (struct msqid_ds *) IPC_UNUSED;
	msgbytes = msghdrs = msg_seq = max_msqid = used_queues = 0;
	msg_lock = NULL;
	return;
}
// 根据用户传进来的数据，新建一个消息节点，然后插入到现在的消息节点链表中去。
int sys_msgsnd (int msqid, struct msgbuf *msgp, int msgsz, int msgflg)
{
	int id, err;
	struct msqid_ds *msq;
	struct ipc_perm *ipcp;
	struct msg *msgh;
	long mtype;
	// 一系列的参数检验	
	if (msgsz > MSGMAX || msgsz < 0 || msqid < 0)
		return -EINVAL;
	if (!msgp) 
		return -EFAULT;
	err = verify_area (VERIFY_READ, msgp->mtext, msgsz);
	if (err) 
		return err;
	if ((mtype = get_fs_long (&msgp->mtype)) < 1)
		return -EINVAL;
	// / seq * MSGMNI + id == msqid
	id = (unsigned int) msqid % MSGMNI;
	msq = msgque [id];
	if (msq == IPC_UNUSED || msq == IPC_NOID)
		return -EINVAL;
	ipcp = &msq->msg_perm; 

 slept:
	// seq * MSGMNI + id == msqid
	if (msq->msg_perm.seq != (unsigned int) msqid / MSGMNI) 
		return -EIDRM;
	// 检查写权限
	if (ipcperms(ipcp, S_IWUGO)) 
		return -EACCES;
	// 要写的大小+现在的大小 > 消息队列的限制大小
	if (msgsz + msq->msg_cbytes > msq->msg_qbytes) { 
		/* no space in queue */
		// 设置了非阻塞则直接返回
		if (msgflg & IPC_NOWAIT)
			return -EAGAIN;
		if (current->signal & ~current->blocked)
			return -EINTR;
		// 睡眠在该消息队列的写阻塞队列
		interruptible_sleep_on (&msq->wwait);
		goto slept;
	}
	
	/* allocate message header and text space*/ 
	// 分配一个新的消息节点，需要多分配msgsz个字节的内存用于存消息内容
	msgh = (struct msg *) kmalloc (sizeof(*msgh) + msgsz, GFP_USER);
	if (!msgh)
		return -ENOMEM;
	// 消息内容的存储内存地址是msg结构体最后一个字节+1
	msgh->msg_spot = (char *) (msgh + 1);
	// 把消息内容复制到内核
	memcpy_fromfs (msgh->msg_spot, msgp->mtext, msgsz); 
	// 无效	
	if (msgque[id] == IPC_UNUSED || msgque[id] == IPC_NOID
		|| msq->msg_perm.seq != (unsigned int) msqid / MSGMNI) {
		kfree(msgh);
		return -EIDRM;
	}

	msgh->msg_next = NULL;
	// 当前的消息节点是当前第一个节点，则让头尾指针指向他
	if (!msq->msg_first)
		msq->msg_first = msq->msg_last = msgh;
	else {
		// 尾插法让新加的消息节点成为最后一个节点，并且更新msg_las指针指向最后一个节点
		msq->msg_last->msg_next = msgh;
		msq->msg_last = msgh;
	}
	// 大小、类型，
	msgh->msg_ts = msgsz;
	msgh->msg_type = mtype;
	// 当前消息队列内容的字节数
	msq->msg_cbytes += msgsz;
	// 所有消息队列的字节数总和增加msgsz个字节
	msgbytes  += msgsz;
	msghdrs++;
	// 该消息队列中的消息节点数加一
	msq->msg_qnum++;
	// 最后一个发送进程
	msq->msg_lspid = current->pid;
	msq->msg_stime = CURRENT_TIME;
	// 如果有进程因为读而被阻塞，则唤醒他们
	if (msq->rwait)
		wake_up (&msq->rwait);
	return msgsz;
}
// 根据条件进行接收消息
int sys_msgrcv (int msqid, struct msgbuf *msgp, int msgsz, long msgtyp, 
		int msgflg)
{
	struct msqid_ds *msq;
	struct ipc_perm *ipcp;
	struct msg *tmsg, *leastp = NULL;
	struct msg *nmsg = NULL;
	int id, err;

	if (msqid < 0 || msgsz < 0)
		return -EINVAL;
	if (!msgp || !msgp->mtext)
	    return -EFAULT;
	err = verify_area (VERIFY_WRITE, msgp->mtext, msgsz);
	if (err)
		return err;

	id = (unsigned int) msqid % MSGMNI;
	msq = msgque [id];
	if (msq == IPC_NOID || msq == IPC_UNUSED)
		return -EINVAL;
	ipcp = &msq->msg_perm; 

	/* 
	 *  find message of correct type.
	 *  msgtyp = 0 => get first.
	 *  msgtyp > 0 => get first message of matching type.
	 *  msgtyp < 0 => get message with least type must be < abs(msgtype).  
	 */
	while (!nmsg) {
		if (msq->msg_perm.seq != (unsigned int) msqid / MSGMNI)
			return -EIDRM;
		if (ipcperms (ipcp, S_IRUGO))
			return -EACCES;
		// 读取消息队列中第一个节点
		if (msgtyp == 0) 
			nmsg = msq->msg_first;
		// 找出第一个符合条件的节点
		else if (msgtyp > 0) {
			// 设置了except标记说明不在节点的type不等于msgtype时满足条件
			if (msgflg & MSG_EXCEPT) { 
				for (tmsg = msq->msg_first; tmsg; 
				     tmsg = tmsg->msg_next)
					if (tmsg->msg_type != msgtyp)
						break;
				// 找不到则为空
				nmsg = tmsg;
			} else {
				// 默认条件是等于
				for (tmsg = msq->msg_first; tmsg; 
				     tmsg = tmsg->msg_next)
					if (tmsg->msg_type == msgtyp)
						break;
				nmsg = tmsg;
			}
		} else {
			for (leastp = tmsg = msq->msg_first; tmsg; 
			     tmsg = tmsg->msg_next) 
				// 找出消息队列中类型值最小的
				if (tmsg->msg_type < leastp->msg_type) 
					leastp = tmsg;
			// 找出消息队列里，type的值小于-msgtype的节点中最小值
			if (leastp && leastp->msg_type <= - msgtyp)
				nmsg = leastp;
		}
		// 找到一个节点
		if (nmsg) { /* done finding a message */
			// 消息节点的字节数比要读的多，并且没有设置MSG_NOERROR标记位则报错
			if ((msgsz < nmsg->msg_ts) && !(msgflg & MSG_NOERROR))
				return -E2BIG;
			/*
				计算需要读取的字节数
				1 消息节点的数据大小比要读的多但是设置了noerror标记，则读取要读的大小，而不是整个数据
				2 节点的数据大小小于要读的大小则读取节点的大小个字节的数据
			*/
			msgsz = (msgsz > nmsg->msg_ts)? nmsg->msg_ts : msgsz;
			// 如果读取的是第一个字节，则需要更新头指针
			if (nmsg ==  msq->msg_first)
				msq->msg_first = nmsg->msg_next;
			else {
				// 遍历消息节点，找到符合条件的节点的前一个节点，然后删除符合条件的节点	
				for (tmsg = msq->msg_first; tmsg; 
				     tmsg = tmsg->msg_next)
					if (tmsg->msg_next == nmsg) 
						break;
				tmsg->msg_next = nmsg->msg_next;
				// 被删除的是最后一个节点，则更新尾指针
				if (nmsg == msq->msg_last)
					msq->msg_last = tmsg;
			}
			// 该消息队列的节点数减一，如果没有节点了则更新头尾指针
			if (!(--msq->msg_qnum))
				msq->msg_last = msq->msg_first = NULL;
			// 设置读取的时间
			msq->msg_rtime = CURRENT_TIME;
			// 设置最后读取消息的进程
			msq->msg_lrpid = current->pid;
			
			msgbytes -= nmsg->msg_ts; 
			msghdrs--; 
			//  一个消息队列中，当前数据的字节大小减去刚被读取的大小
			msq->msg_cbytes -= nmsg->msg_ts;
			// 如果有进程阻塞在写队列则唤醒他
			if (msq->wwait)
				wake_up (&msq->wwait);
			put_fs_long (nmsg->msg_type, &msgp->mtype);
			memcpy_tofs (msgp->mtext, nmsg->msg_spot, msgsz);
			kfree(nmsg);
			return msgsz;
		} else {  /* did not find a message */
			// 非阻塞调用直接返回
			if (msgflg & IPC_NOWAIT)
				return -ENOMSG;
			if (current->signal & ~current->blocked)
				return -EINTR; 
			// 阻塞在写队列，等待写入消息
			interruptible_sleep_on (&msq->rwait);
		}
	} /* end while */
	return -1;
}


static int findkey (key_t key)
{
	int id;
	struct msqid_ds *msq;
	
	for (id = 0; id <= max_msqid; id++) {
		// 还没有分配内存，则睡眠，等待分配内存后被唤醒，见newque函数
		while ((msq = msgque[id]) == IPC_NOID) 
			interruptible_sleep_on (&msg_lock);
		// 走到这说明状态是可使用或因为还没有分配内存两种情况
		if (msq == IPC_UNUSED)
			continue;
		// 键一样则返回
		if (key == msq->msg_perm.key)
			return id;
	}
	return -1;
}

static int newque (key_t key, int msgflg)
{
	int id;
	struct msqid_ds *msq;
	struct ipc_perm *ipcp;

	for (id = 0; id < MSGMNI; id++) 
		// 找到一个还没有使用的项
		if (msgque[id] == IPC_UNUSED) {
			// 设置成还没有分配内存状态
			msgque[id] = (struct msqid_ds *) IPC_NOID;
			goto found;
		}
	return -ENOSPC;

found:
	msq = (struct msqid_ds *) kmalloc (sizeof (*msq), GFP_KERNEL);
	if (!msq) {
		msgque[id] = (struct msqid_ds *) IPC_UNUSED;
		if (msg_lock)
			wake_up (&msg_lock);
		return -ENOMEM;
	}
	// 初始化权限相关的结构
	ipcp = &msq->msg_perm;
	ipcp->mode = (msgflg & S_IRWXUGO);
	ipcp->key = key;
	ipcp->cuid = ipcp->uid = current->euid;
	ipcp->gid = ipcp->cgid = current->egid;
	msq->msg_perm.seq = msg_seq;
	msq->msg_first = msq->msg_last = NULL;
	msq->rwait = msq->wwait = NULL;
	msq->msg_cbytes = msq->msg_qnum = 0;
	msq->msg_lspid = msq->msg_lrpid = 0;
	msq->msg_stime = msq->msg_rtime = 0;
	msq->msg_qbytes = MSGMNB;
	msq->msg_ctime = CURRENT_TIME;
	// 判断和保存当前最大id
	if (id > max_msqid)
		max_msqid = id;
	msgque[id] = msq;
	// 系统消息队列数加一
	used_queues++;
	// 唤醒因为没有内存而阻塞的队列
	if (msg_lock)
		wake_up (&msg_lock);
	// 通过seq和id两个变量更好地防止id回环导致的问题
	return (unsigned int) msq->msg_perm.seq * MSGMNI + id;
}
// 查找获取申请一个消息队列
int sys_msgget (key_t key, int msgflg)
{
	int id;
	struct msqid_ds *msq;
	// 设置了私有的标记则直接创建一个新的消息队列	
	if (key == IPC_PRIVATE) 
		return newque(key, msgflg);
	// 找不到
	if ((id = findkey (key)) == -1) { /* key not used */
		// 有传IPC_CREAT标记则创建一个新的队列，否则返回找不到
		if (!(msgflg & IPC_CREAT))
			return -ENOENT;
		return newque(key, msgflg);
	}
	// 找到了，但是设置了下面两个标记位说明该消息队列需要由当前进程创建才会返回成功
	if (msgflg & IPC_CREAT && msgflg & IPC_EXCL)
		return -EEXIST;
	msq = msgque[id];
	// 无效
	if (msq == IPC_UNUSED || msq == IPC_NOID)
		return -EIDRM;
	// 检查权限
	if (ipcperms(&msq->msg_perm, msgflg))
		return -EACCES;
	return (unsigned int) msq->msg_perm.seq * MSGMNI + id;
} 
// 移除id对应的消息队列
static void freeque (int id)
{
	struct msqid_ds *msq = msgque[id];
	struct msg *msgp, *msgh;

	msq->msg_perm.seq++;
	// 递增序列号，防止id回环可能会有问题
	msg_seq = (msg_seq+1) % ((unsigned)(1<<31)/MSGMNI); /* increment, but avoid overflow */
	// 更新全部消息队列的字节数总和
	msgbytes -= msq->msg_cbytes;
	// 如果id是消息队列的最大id，则需要更新，即从大往小遍历，找到第一个在使用的项，该项的id就是当前最大id
	if (id == max_msqid)
		while (max_msqid && (msgque[--max_msqid] == IPC_UNUSED));
	msgque[id] = (struct msqid_ds *) IPC_UNUSED;
	used_queues--;
	// 准备销毁该消息队列，需要唤醒被阻塞的进程，否则他一直等待
	while (msq->rwait || msq->wwait) {
		if (msq->rwait)
			wake_up (&msq->rwait); 
		if (msq->wwait)
			wake_up (&msq->wwait);
		schedule(); 
	}
	// 释放该消息队列上所有的消息节点
	for (msgp = msq->msg_first; msgp; msgp = msgh ) {
		msgh = msgp->msg_next;
		msghdrs--;
		kfree(msgp);
	}
	kfree(msq);
}
// 对msqid对应的消息队列进行增删改查操作
int sys_msgctl (int msqid, int cmd, struct msqid_ds *buf)
{
	int id, err;
	struct msqid_ds *msq;
	struct msqid_ds tbuf;
	struct ipc_perm *ipcp;
	
	if (msqid < 0 || cmd < 0)
		return -EINVAL;
	switch (cmd) {
	case IPC_INFO: 
	case MSG_INFO: 
		if (!buf)
			return -EFAULT;
	{ 
		struct msginfo msginfo;
		msginfo.msgmni = MSGMNI;
		msginfo.msgmax = MSGMAX;
		msginfo.msgmnb = MSGMNB;
		msginfo.msgmap = MSGMAP;
		msginfo.msgpool = MSGPOOL;
		msginfo.msgtql = MSGTQL;
		msginfo.msgssz = MSGSSZ;
		msginfo.msgseg = MSGSEG;
		if (cmd == MSG_INFO) {
			msginfo.msgpool = used_queues;
			msginfo.msgmap = msghdrs;
			msginfo.msgtql = msgbytes;
		}
		err = verify_area (VERIFY_WRITE, buf, sizeof (struct msginfo));
		if (err)
			return err;
		memcpy_tofs (buf, &msginfo, sizeof(struct msginfo));
		return max_msqid;
	}
	case MSG_STAT:
		if (!buf)
			return -EFAULT;
		err = verify_area (VERIFY_WRITE, buf, sizeof (*buf));
		if (err)
			return err;
		if (msqid > max_msqid)
			return -EINVAL;
		msq = msgque[msqid];
		if (msq == IPC_UNUSED || msq == IPC_NOID)
			return -EINVAL;
		if (ipcperms (&msq->msg_perm, S_IRUGO))
			return -EACCES;
		id = (unsigned int) msq->msg_perm.seq * MSGMNI + msqid;
		tbuf.msg_perm   = msq->msg_perm;
		tbuf.msg_stime  = msq->msg_stime;
		tbuf.msg_rtime  = msq->msg_rtime;
		tbuf.msg_ctime  = msq->msg_ctime;
		tbuf.msg_cbytes = msq->msg_cbytes;
		tbuf.msg_qnum   = msq->msg_qnum;
		tbuf.msg_qbytes = msq->msg_qbytes;
		tbuf.msg_lspid  = msq->msg_lspid;
		tbuf.msg_lrpid  = msq->msg_lrpid;
		memcpy_tofs (buf, &tbuf, sizeof(*buf));
		return id;
	case IPC_SET:
		if (!buf)
			return -EFAULT;
		err = verify_area (VERIFY_READ, buf, sizeof (*buf));
		if (err)
			return err;
		memcpy_fromfs (&tbuf, buf, sizeof (*buf));
		break;
	case IPC_STAT:
		if (!buf)
			return -EFAULT;
		err = verify_area (VERIFY_WRITE, buf, sizeof(*buf));
		if (err)
			return err;
		break;
	}

	id = (unsigned int) msqid % MSGMNI;
	msq = msgque [id];
	if (msq == IPC_UNUSED || msq == IPC_NOID)
		return -EINVAL;
	if (msq->msg_perm.seq != (unsigned int) msqid / MSGMNI)
		return -EIDRM;
	ipcp = &msq->msg_perm;

	switch (cmd) {
	case IPC_STAT:
		if (ipcperms (ipcp, S_IRUGO))
			return -EACCES;
		tbuf.msg_perm   = msq->msg_perm;
		tbuf.msg_stime  = msq->msg_stime;
		tbuf.msg_rtime  = msq->msg_rtime;
		tbuf.msg_ctime  = msq->msg_ctime;
		tbuf.msg_cbytes = msq->msg_cbytes;
		tbuf.msg_qnum   = msq->msg_qnum;
		tbuf.msg_qbytes = msq->msg_qbytes;
		tbuf.msg_lspid  = msq->msg_lspid;
		tbuf.msg_lrpid  = msq->msg_lrpid;
		memcpy_tofs (buf, &tbuf, sizeof (*buf));
		return 0;
	case IPC_SET:
		if (!suser() && current->euid != ipcp->cuid && 
		    current->euid != ipcp->uid)
			return -EPERM;
		if (tbuf.msg_qbytes > MSGMNB && !suser())
			return -EPERM;
		msq->msg_qbytes = tbuf.msg_qbytes;
		ipcp->uid = tbuf.msg_perm.uid;
		ipcp->gid =  tbuf.msg_perm.gid;
		ipcp->mode = (ipcp->mode & ~S_IRWXUGO) | 
			(S_IRWXUGO & tbuf.msg_perm.mode);
		msq->msg_ctime = CURRENT_TIME;
		return 0;
	case IPC_RMID:
		if (!suser() && current->euid != ipcp->cuid && 
		    current->euid != ipcp->uid)
			return -EPERM;
		freeque (id); 
		return 0;
	default:
		return -EINVAL;
	}
}
