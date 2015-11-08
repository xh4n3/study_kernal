/*
 *  linux/kernel/exit.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/tty.h>
#include <asm/segment.h>

int sys_pause(void);
int sys_close(int fd);

void release(struct task_struct * p)
{
	int i;

	if (!p)
		return;
	for (i=1 ; i<NR_TASKS ; i++)
		if (task[i]==p) {
			task[i]=NULL;
			free_page((long)p);
			schedule();
			return;
		}
	panic("trying to release non-existent task");
}

static inline int send_sig(long sig,struct task_struct * p,int priv)
{
	if (!p || sig<1 || sig>32)
		return -EINVAL;
	if (priv || (current->euid==p->euid) || suser())
		p->signal |= (1<<(sig-1));
	else
		return -EPERM;
	return 0;
}

static void kill_session(void)
{
	struct task_struct **p = NR_TASKS + task;
	// 循环所有进程，给 session 一致的所有进程发送 SIGHUP
	while (--p > &FIRST_TASK) {
		if (*p && (*p)->session == current->session)
			(*p)->signal |= 1<<(SIGHUP-1);
	}
}

/*
 * XXX need to check permissions needed to send signals to process
 * groups, etc. etc.  kill() permissions semantics are tricky!
 */
int sys_kill(int pid,int sig)
{
	struct task_struct **p = NR_TASKS + task;
	int err, retval = 0;
	// (*p)->pgrp == current->pid 说明 p 为 current 本身或者由 current 启动的进程组中进程之一
	// pid = 0 时强制给自身或者由自身引领的一系列进程发送该信号
	// Shell 中一条管道命令，如果是 ps | cat，cat 的 pgrp 就等于 ps 的 pid，它们同属一个进程组
	if (!pid) while (--p > &FIRST_TASK) {
		if (*p && (*p)->pgrp == current->pid) 
			if ((err=send_sig(sig,*p,1)))
				retval = err;
	} else if (pid>0) while (--p > &FIRST_TASK) {
		// pid > 0 时给 pid 指定的进程发送信号，此处会检查进程有效用户是否与当前进程有效用户一致，或者是否 root
		if (*p && (*p)->pid == pid) 
			if ((err=send_sig(sig,*p,0)))
				retval = err;
	} else if (pid == -1) while (--p > &FIRST_TASK) {
			// pid == -1 时，给所有进程发送信号
			// If superuser, broadcast the signal to all processes;
			// otherwise broadcast to all processes belonging to the user.
		if ((err = send_sig(sig,*p,0)))
			retval = err;
	} else while (--p > &FIRST_TASK)
			// pid 为除 -1 以外负值时，给绝对值满足 pid 的发送信号，此时为非强制性发送
		if (*p && (*p)->pgrp == -pid)
			if ((err = send_sig(sig,*p,0)))
				retval = err;
	return retval;
}

static void tell_father(int pid)
{
	int i;
	// 循环所有进程，给父进程 pid 发送 SIGCHLD 信号
	if (pid)
		for (i=0;i<NR_TASKS;i++) {
			if (!task[i])
				continue;
			if (task[i]->pid != pid)
				continue;
			task[i]->signal |= (1<<(SIGCHLD-1));
			return;
		}
/* if we don't find any fathers, we just release ourselves */
/* This is not really OK. Must change it to make father 1 */
	// 如果实在找不到父进程，就把自己的进程释放了
	printk("BAD BAD - no father found\n\r");
	// 把自己从进程列表 task 中删除，并且调用 schedule()
	release(current);
}

int do_exit(long code)
{
	int i;
	// TODO 待看页表
	free_page_tables(get_base(current->ldt[1]),get_limit(0x0f));
	free_page_tables(get_base(current->ldt[2]),get_limit(0x17));
	/*
	 * 找出所有子进程，将它们的父亲设为 init 进程，将状态标记为僵尸
	 * 然后给 init 进程发送一个 SIGCHLD 信号，提醒 init 进程回收子进程
	 */
	for (i=0 ; i<NR_TASKS ; i++)
		if (task[i] && task[i]->father == current->pid) {
			task[i]->father = 1;
			if (task[i]->state == TASK_ZOMBIE)
				/* assumption task[1] is always init */
				// 最后一个参数 1 代表 privilege，此处为强制发送
				(void) send_sig(SIGCHLD, task[1], 1);
		}
	// TODO 关闭文件？
	/*
	 * NR_OPEN 是一个进程可以打开的最大文件数
	 * 而 NR_FILE 是系统在某时刻的限制文件总数
	 */
	for (i=0 ; i<NR_OPEN ; i++)
		if (current->filp[i])
			sys_close(i);
	// 进程的当前工作目录 inode
	iput(current->pwd);
	current->pwd=NULL;
	// 进程的根目录 inode
	iput(current->root);
	current->root=NULL;
	// 进程本身可执行文件的 inode
	iput(current->executable);
	current->executable=NULL;
	if (current->leader && current->tty >= 0)
		tty_table[current->tty].pgrp = 0;
	if (last_task_used_math == current)
		last_task_used_math = NULL;
	/*
	 * 如果是 session leader 会话领头进程，则向该会话所有进程发送 SIGHUP 信号
	 * PID, PPID, PGID, SID
	 * http://unix.stackexchange.com/questions/18166/what-are-session-leaders-in-ps
	 * 在同一次 ssh 会话中，用户对应的 shell 最先被启动，成为 session leader，
	 * 所有在同一次会话中产生的进程 session id 都等于这个 session leader 的 pid
	 * 当 session leader 退出时，它会向所有同一 session 中的进程发送 SIGHUP，
	 * 这个信号是可以被捕获的，如果进程忽略这个 SIGHUP，它则可以以一个孤儿进程继续执行
	 * http://www.firefoxbug.com/index.php/archives/2782/
	 */
	if (current->leader)
		kill_session();
	// 将自己设为僵尸，设置退出状态码，同时告诉父进程回收子进程
	current->state = TASK_ZOMBIE;
	current->exit_code = code;
	tell_father(current->father);
	// 如果 tell_father 中找不到父进程，自己把自己释放掉了，那也不会有机会继续执行下面代码了
	schedule();
	return (-1);	/* just to suppress warnings */
}

int sys_exit(int error_code)
{
	return do_exit((error_code&0xff)<<8);
}

int sys_waitpid(pid_t pid,unsigned long * stat_addr, int options)
{
	int flag, code;
	struct task_struct ** p;
	// TODO
	verify_area(stat_addr,4);
repeat:
	flag=0;
	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		// p 空或者为本进程则跳过
		if (!*p || *p == current)
			continue;
		// 非本进程的子进程也跳过
		if ((*p)->father != current->pid)
			continue;
		// 当 pid 大于 0 时，说明明确等待某一子进程，则跳过不匹配的子进程
		if (pid>0) {
			if ((*p)->pid != pid)
				continue;
		// 如果 pid 等于 0， 则等待同进程组，所以跳过非同组的
		} else if (!pid) {
			if ((*p)->pgrp != current->pgrp)
				continue;
		// 等待绝对值
		} else if (pid != -1) {
			if ((*p)->pgrp != -pid)
				continue;
		}
		// 如果 pid = -1，则等待任何子进程
		switch ((*p)->state) {
			// TODO
			case TASK_STOPPED:
				if (!(options & WUNTRACED))
					continue;
				put_fs_long(0x7f,stat_addr);
				return (*p)->pid;
			case TASK_ZOMBIE:
				//  如果为僵尸进程，将子进程的用户态滴答数加到该进程用户态滴答数中
				current->cutime += (*p)->utime;
				// 内核态滴答数也要加上
				current->cstime += (*p)->stime;
				// 将该进程 pid 返回
				flag = (*p)->pid;
				code = (*p)->exit_code;
				// 释放该进程
				release(*p);
				// TODO 不知道这个是什么意思
				put_fs_long(code,stat_addr);
				return flag;
			default:
				flag=1;
				continue;
		}
	}
	if (flag) {
		if (options & WNOHANG)
			return 0;
		current->state=TASK_INTERRUPTIBLE;
		schedule();
		if (!(current->signal &= ~(1<<(SIGCHLD-1))))
			goto repeat;
		else
			return -EINTR;
	}
	return -ECHILD;
}


