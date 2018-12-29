/*
 *  linux/kernel/signal.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/wait.h>
#include <linux/ptrace.h>
#include <linux/unistd.h>
#include <linux/mm.h>

#include <asm/segment.h>
// 某位置位
#define _S(nr) (1<<((nr)-1))
// kill和stop信号不能屏蔽，取反后即为可以屏蔽的位
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))

// 设置进程信号屏蔽位，how代表操作类型，set代表新的屏蔽位，效果取决于how，oset保存旧的屏蔽位
asmlinkage int sys_sigprocmask(int how, sigset_t *set, sigset_t *oset)
{
	sigset_t new_set, old_set = current->blocked; // 获取旧的信号屏蔽位
	int error;
	// 如果没有传set则相当于get，获取旧的屏蔽位
	if (set) {
		error = verify_area(VERIFY_READ, set, sizeof(sigset_t));
		if (error)
			return error;
		new_set = get_fs_long((unsigned long *) set) & _BLOCKABLE; // 校正数据
		// 操作类型分别是新增，删除，覆盖
		switch (how) {
		case SIG_BLOCK:
			current->blocked |= new_set;
			break;
		case SIG_UNBLOCK:
			current->blocked &= ~new_set;
			break;
		case SIG_SETMASK:
			current->blocked = new_set;
			break;
		default:
			return -EINVAL;
		}
	}
	// 如果oset非空，则把旧的屏蔽位存储到oset中返回
	if (oset) {
		error = verify_area(VERIFY_WRITE, oset, sizeof(sigset_t));
		if (error)
			return error;
		put_fs_long(old_set, (unsigned long *) oset);
	}
	return 0;
}

// 获取进程屏蔽位
asmlinkage int sys_sgetmask(void)
{
	return current->blocked;
}

// 设置进程屏蔽位
asmlinkage int sys_ssetmask(int newmask)
{
	int old=current->blocked;

	current->blocked = newmask & _BLOCKABLE;
	return old;
}

// 获取进程当前的收到的被阻塞的信号量
asmlinkage int sys_sigpending(sigset_t *set)
{
	int error;
	/* fill in "set" with signals pending but blocked. */
	error = verify_area(VERIFY_WRITE, set, 4);
	if (!error)
		put_fs_long(current->blocked & current->signal, (unsigned long *)set);
	return error;
}

/*
 * POSIX 3.3.1.3:
 *  "Setting a signal action to SIG_IGN for a signal that is pending
 *   shall cause the pending signal to be discarded, whether or not
 *   it is blocked" (but SIGCHLD is unspecified: linux leaves it alone).
 *
 *  "Setting a signal action to SIG_DFL for a signal that is pending
 *   and whose default action is to ignore the signal (for example,
 *   SIGCHLD), shall cause the pending signal to be discarded, whether
 *   or not it is blocked"
 *
 * Note the silly behaviour of SIGCHLD: SIG_IGN means that the signal
 * isn't actually ignored, but does automatic child reaping, while
 * SIG_DFL is explicitly said by POSIX to force the signal to be ignored..
 */
static void check_pending(int signum)
{
	struct sigaction *p;
	// 根据信号的值获取对应的处理结构
	p = signum - 1 + current->sigaction;
	// 如果对信号的处理是忽略
	if (p->sa_handler == SIG_IGN) {
		// 如果信号是子进程退出则直接返回
		if (signum == SIGCHLD)
			return;
		// 否则清除该信号
		current->signal &= ~_S(signum);
		return;
	}
	// 如果信号的处理是默认行为
	if (p->sa_handler == SIG_DFL) {
		// 信号不等于下面这几个则返回
		if (signum != SIGCONT && signum != SIGCHLD && signum != SIGWINCH)
			return;
		// 其他的则清除对应的位
		current->signal &= ~_S(signum);
		return;
	}	
}
// 给某个信号设置处理函数
asmlinkage unsigned long sys_signal(int signum, void (*handler)(int))
{
	int err;
	struct sigaction tmp;

	if (signum<1 || signum>32)
		return -EINVAL;
	if (signum==SIGKILL || signum==SIGSTOP)
		return -EINVAL;
	if (handler != SIG_DFL && handler != SIG_IGN) {
		err = verify_area(VERIFY_READ, handler, 1);
		if (err)
			return err;
	}
	tmp.sa_handler = handler;
	tmp.sa_mask = 0;
	tmp.sa_flags = SA_ONESHOT | SA_NOMASK;
	tmp.sa_restorer = NULL;
	// 保存旧的处理函数
	handler = current->sigaction[signum-1].sa_handler;
	// 设置新的处理函数
	current->sigaction[signum-1] = tmp;
	check_pending(signum);
	return (unsigned long) handler;
}
// 类似上面
asmlinkage int sys_sigaction(int signum, const struct sigaction * action,
	struct sigaction * oldaction)
{
	struct sigaction new_sa, *p;

	if (signum<1 || signum>32)
		return -EINVAL;
	if (signum==SIGKILL || signum==SIGSTOP)
		return -EINVAL;
	p = signum - 1 + current->sigaction;
	if (action) {
		int err = verify_area(VERIFY_READ, action, sizeof(*action));
		if (err)
			return err;
		memcpy_fromfs(&new_sa, action, sizeof(struct sigaction));
		if (new_sa.sa_flags & SA_NOMASK)
			new_sa.sa_mask = 0;
		else {
			new_sa.sa_mask |= _S(signum);
			new_sa.sa_mask &= _BLOCKABLE;
		}
		if (new_sa.sa_handler != SIG_DFL && new_sa.sa_handler != SIG_IGN) {
			err = verify_area(VERIFY_READ, new_sa.sa_handler, 1);
			if (err)
				return err;
		}
	}
	if (oldaction) {
		int err = verify_area(VERIFY_WRITE, oldaction, sizeof(*oldaction));
		if (err)
			return err;
		memcpy_tofs(oldaction, p, sizeof(struct sigaction));
	}
	if (action) {
		*p = new_sa;
		check_pending(signum);
	}
	return 0;
}
