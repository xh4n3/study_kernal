/*
 *  linux/kernel/blk_dev/ll_rw.c
 *
 * (C) 1991 Linus Torvalds
 */
/*
 * 当程序需要读取硬盘上的逻辑块时，首先向缓冲区管理程序提出申请，而程序进程本身进入睡眠等待状态，缓冲区管理程序会先在
 * 缓冲区中查找以前是否已经读取过该数据，如果是则直接将对应的缓冲区头部指针返回并唤醒该程序，
 * 否则才会调用此文件 ll_rw_blk.c，其中 ll 意思就是 low level，低级接口，然后 ll_rw_blk 函数会向相应的块设备驱动程序发出一个读写
 * 数据块的请求。这个函数为此创建一个请求结构项，插入到请求队列中。
 * 在插入请求项时使用了电梯算法减少设备磁头移动。
 *
 * 电梯调度算法如下：
 * 总是从磁臂当前位置开始，沿磁臂的移动方向去选择离当前磁臂最近的那个柱面的访问者。
 * 如果沿磁臂的方向无请求访问时，就改变磁臂的移动方向。采用这种调度算法，需要为访问者设置两个队列，根据磁头的移动方向，
 * 能访问到的访问者由近及远排队，背离磁头移动方向的访问者也由近及远排为另一队。先按磁头移动方向队列调度访问者访问磁盘，
 * 当该方向没有访问者时，再改变方向，选择另一个访问者队列访问磁盘。
 * 假设请求调度的磁道为 98, 183, 37, 122, 14, 124, 65, 67，而磁头当前在 65 号上，磁头会按照 65, 67, 98, 122, 124, 183,
 * 37,14 的顺序依次查找，并输入到内存中。
 *
 * 每次访盘请求：
 * 读/写，磁盘地址（设备号【多个磁盘设备情况下】，柱面号【其实就是磁道号】，磁头号【哪个盘面】，扇区号【每个扇区有500多个字节】，
 * 内存地址（源/目的）
 * 参考视频：
 * Coursera 操作系统原理
 * https://class.coursera.org/os-001/lecture/115
 * http://v.ku6.com/show/zCTB6Fmc2HGRPxko.html
 * 复制文件时，磁头可能在不同磁道中转换，每次复制和粘贴其实是从某磁道读数据到内存，然后再从内存写数据到另一磁道的过程
 * 所以这时候拷贝过程的缓存设置大小直接决定了磁头转换磁道的次数，从而影响复制拷贝时间
 * http://v.17173.com/v_102_604/MjQ0NTgzMjI.html
 * 磁头根据磁盘中扇区的磁性信息来得出二进制串
 */
/*
 * This handles all read/write requests to block devices
 */
#include <errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include "blk.h"

/*
 * The request-struct contains all necessary data
 * to load a nr of sectors into memory
 */
struct request request[NR_REQUEST];

/*
 * used to wait on when there are no free requests
 */
struct task_struct * wait_for_request = NULL;

/* blk_dev_struct is:
 *	do_request-address
 *	next-request
 */
struct blk_dev_struct blk_dev[NR_BLK_DEV] = {
	{ NULL, NULL },		/* no_dev */
	{ NULL, NULL },		/* dev mem */
	{ NULL, NULL },		/* dev fd */
	{ NULL, NULL },		/* dev hd */
	{ NULL, NULL },		/* dev ttyx */
	{ NULL, NULL },		/* dev tty */
	{ NULL, NULL }		/* dev lp */
};

static inline void lock_buffer(struct buffer_head * bh)
{
	cli();
	while (bh->b_lock)
		sleep_on(&bh->b_wait);
	bh->b_lock=1;
	sti();
}

static inline void unlock_buffer(struct buffer_head * bh)
{
	if (!bh->b_lock)
		printk("ll_rw_block.c: buffer not locked\n\r");
	bh->b_lock = 0;
	wake_up(&bh->b_wait);
}

/*
 * add-request adds a request to the linked list.
 * It disables interrupts so that it can muck with the
 * request-lists in peace.
 */
static void add_request(struct blk_dev_struct * dev, struct request * req)
{
	struct request * tmp;

	req->next = NULL;
	cli();
	if (req->bh)
		req->bh->b_dirt = 0;
	if (!(tmp = dev->current_request)) {
		dev->current_request = req;
		sti();
		(dev->request_fn)();
		return;
	}
	for ( ; tmp->next ; tmp=tmp->next)
		if ((IN_ORDER(tmp,req) || 
		    !IN_ORDER(tmp,tmp->next)) &&
		    IN_ORDER(req,tmp->next))
			break;
	req->next=tmp->next;
	tmp->next=req;
	sti();
}

static void make_request(int major,int rw, struct buffer_head * bh)
{
	struct request * req;
	int rw_ahead;

/* WRITEA/READA is special case - it is not really needed, so if the */
/* buffer is locked, we just forget about it, else it's a normal read */
	if ((rw_ahead = (rw == READA || rw == WRITEA))) {
		if (bh->b_lock)
			return;
		if (rw == READA)
			rw = READ;
		else
			rw = WRITE;
	}
	if (rw!=READ && rw!=WRITE)
		panic("Bad block dev command, must be R/W/RA/WA");
	lock_buffer(bh);
	if ((rw == WRITE && !bh->b_dirt) || (rw == READ && bh->b_uptodate)) {
		unlock_buffer(bh);
		return;
	}
repeat:
/* we don't allow the write-requests to fill up the queue completely:
 * we want some room for reads: they take precedence. The last third
 * of the requests are only for reads.
 */
	if (rw == READ)
		req = request+NR_REQUEST;
	else
		req = request+((NR_REQUEST*2)/3);
/* find an empty request */
	while (--req >= request)
		if (req->dev<0)
			break;
/* if none found, sleep on new requests: check for rw_ahead */
	if (req < request) {
		if (rw_ahead) {
			unlock_buffer(bh);
			return;
		}
		sleep_on(&wait_for_request);
		goto repeat;
	}
/* fill up the request-info, and add it to the queue */
	req->dev = bh->b_dev;
	req->cmd = rw;
	req->errors=0;
	req->sector = bh->b_blocknr<<1;
	req->nr_sectors = 2;
	req->buffer = bh->b_data;
	req->waiting = NULL;
	req->bh = bh;
	req->next = NULL;
	add_request(major+blk_dev,req);
}

void ll_rw_block(int rw, struct buffer_head * bh)
{
	unsigned int major;

	if ((major=MAJOR(bh->b_dev)) >= NR_BLK_DEV ||
	!(blk_dev[major].request_fn)) {
		printk("Trying to read nonexistent block-device\n\r");
		return;
	}
	make_request(major,rw,bh);
}

void blk_dev_init(void)
{
	int i;

	for (i=0 ; i<NR_REQUEST ; i++) {
		request[i].dev = -1;
		request[i].next = NULL;
	}
}
