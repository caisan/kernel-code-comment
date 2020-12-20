/*
 *  MQ Deadline i/o scheduler - adaptation of the legacy deadline scheduler,
 *  for the blk-mq scheduling framework
 *
 *  Copyright (C) 2016 Jens Axboe <axboe@kernel.dk>
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/rbtree.h>
#include <linux/sbitmap.h>

#include "blk.h"
#include "blk-mq.h"
#include "blk-mq-debugfs.h"
#include "blk-mq-tag.h"
#include "blk-mq-sched.h"

/*
 * See Documentation/block/deadline-iosched.txt
 */
static const int read_expire = HZ / 2;  /* max time before a read is submitted. */
static const int write_expire = 5 * HZ; /* ditto for writes, these limits are SOFT! */
static const int writes_starved = 2;    /* max times reads can starve a write */
static const int fifo_batch = 16;       /* # of sequential requests treated as one
				     by the above parameters. For throughput. */

struct deadline_data {
	/*
	 * run time data
	 */

	/*
	 * requests (deadline_rq s) are present on both sort_list and fifo_list
	 */
	struct rb_root sort_list[2];
	struct list_head fifo_list[2];

	/*
	 * next in sort order. read, write or both are NULL
	 */
	struct request *next_rq[2];
	unsigned int batching;		/* number of sequential requests made */
	unsigned int starved;		/* times reads have starved writes */

	/*
	 * settings that change how the i/o scheduler behaves
	 */
	int fifo_expire[2];
	int fifo_batch;
	int writes_starved;
	int front_merges;

	spinlock_t lock;
    //dd_insert_request�У���req��ӵ���dispatch ����
	struct list_head dispatch;
};

static inline struct rb_root *
deadline_rb_root(struct deadline_data *dd, struct request *rq)
{
	return &dd->sort_list[rq_data_dir(rq)];
}

/*
 * get the request after `rq' in sector-sorted order
 */
static inline struct request *
deadline_latter_request(struct request *rq)
{
	struct rb_node *node = rb_next(&rq->rb_node);

	if (node)
		return rb_entry_rq(node);

	return NULL;
}

static void
deadline_add_rq_rb(struct deadline_data *dd, struct request *rq)
{
	struct rb_root *root = deadline_rb_root(dd, rq);
    //����req�Ĵ�����ʼ��ַ��req��ӵ���������������������req�����й����ǣ�˭�Ĵ�����ʼ��ַС˭����
	elv_rb_add(root, rq);
}
//��dd->next_rq[]��ֵreq����һ��req����һ�δӺ����ѡ��req��������ʱ�õ������Ұ�req�Ӻ�������޳�
static inline void
deadline_del_rq_rb(struct deadline_data *dd, struct request *rq)
{
	const int data_dir = rq_data_dir(rq);

    //��dd->next_rq[]��ֵreq����һ��req,��һ�δӺ����ѡ��req��������ʱ��ֱ��ʹ�����nedd->next_rq[]xt_rq
    if (dd->next_rq[data_dir] == rq)
		dd->next_rq[data_dir] = deadline_latter_request(rq);
    
    //deadline_rb_root(dd, rq)��ȡ�������㷨�Ķ�����д���������ͷrb_root��Ȼ���req���������������޳���
	elv_rb_del(deadline_rb_root(dd, rq), rq);
}

/*
 * remove rq from rbtree and fifo.
 */
static void deadline_remove_request(struct request_queue *q, struct request *rq)
{
	struct deadline_data *dd = q->elevator->elevator_data;

	list_del_init(&rq->queuelist);

	/*
	 * We might not be on the rbtree, if we are doing an insert merge
	 */
	if (!RB_EMPTY_NODE(&rq->rb_node))
		deadline_del_rq_rb(dd, rq);

	elv_rqhash_del(q, rq);
	if (q->last_merge == rq)
		q->last_merge = NULL;
}

static void dd_request_merged(struct request_queue *q, struct request *req,
			      int type)
{
	struct deadline_data *dd = q->elevator->elevator_data;

	/*
	 * if the merge was a front merge, we need to reposition request
	 */
	if (type == ELEVATOR_FRONT_MERGE) {
		elv_rb_del(deadline_rb_root(dd, req), req);
		deadline_add_rq_rb(dd, req);
	}
}
//��fifo�������req�ƶ���next�ڵ��λ�ã�����req�ĳ�ʱʱ�䡣��fifo���кͺ�����޳�next,������dd->next_rq[]��ֵnext����һ��req
static void dd_merged_requests(struct request_queue *q, struct request *req,
			       struct request *next)
{
	/*
	 * if next expires before rq, assign its expire time to rq
	 * and move into next position (next will be deleted) in fifo
	 */
	//���next�ĳ�ʱʱ������req�����µ�req��ʱʱ����
	if (!list_empty(&req->queuelist) && !list_empty(&next->queuelist)) {
		if (time_before((unsigned long)next->fifo_time,
				(unsigned long)req->fifo_time)) {
		    //��req�ƶ���next�ڵ��λ�ã����������fifo����
			list_move(&req->queuelist, &next->queuelist);
            //����req�ĳ�ʱʱ��
			req->fifo_time = next->fifo_time;
		}
	}

	/*
	 * kill knowledge of next, this one is a goner
	 */
	//��fifo�����޳�next,�Ӻ�����޳�next����dd->next_rq[]��ֵnext����һ��req����һ�δӺ����ѡ��req��������ʱ�õ�
	deadline_remove_request(q, next);
}

/*
 * move an entry to dispatch queue
 */
//�Ӻ����������req��next req��dd->next_rq[data_dir],��req��fifo���кͺ���������޳�
static void
deadline_move_request(struct deadline_data *dd, struct request *rq)
{
	const int data_dir = rq_data_dir(rq);

	dd->next_rq[READ] = NULL;
	dd->next_rq[WRITE] = NULL;
    //�Ӻ����������req��next req��dd->next_rq[data_dir]
	dd->next_rq[data_dir] = deadline_latter_request(rq);

	/*
	 * take it off the sort and fifo list
	 */
	//ֻ�а�req��fifo���кͺ���������޳�
	deadline_remove_request(rq->q, rq);
}

/*
 * deadline_check_fifo returns 0 if there are no expired requests on the fifo,
 * 1 otherwise. Requires !list_empty(&dd->fifo_list[data_dir])
 */
static inline int deadline_check_fifo(struct deadline_data *dd, int ddir)
{
	struct request *rq = rq_entry_fifo(dd->fifo_list[ddir].next);

	/*
	 * rq is expired!
	 */
	if (time_after_eq(jiffies, (unsigned long)rq->fifo_time))
		return 1;

	return 0;
}

/*
 * deadline_dispatch_requests selects the best request according to
 * read/write expire, fifo_batch, etc
 */
//mq-deadline�����㷨�����ɷ�req�������ú����뵥ͨ��deadline�ɷ��㷨����deadline_dispatch_requestsԭ�����һ�¡�
//ִ��deadline�ɷ���������fifo���ߺ��������ѡ����ɷ������������req���ء�Ȼ�������µ�next_rq������req��fifo���кͺ���������޳���
//req��Դ��:�ϴ��ɷ����õ�next_rq;read req�ɷ������ѡ���write req;fifo �����ϳ�ʱҪ�����req��ͳ���ˣ��й̶�����
static struct request *__dd_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct deadline_data *dd = hctx->queue->elevator->elevator_data;
	struct request *rq;
	bool reads, writes;
	int data_dir;

	if (!list_empty(&dd->dispatch)) {
        //���dd->dispatch������reqҪ�ɷ���ȡ����ֱ�Ӻ�ֱ�ӷ��أ���ȥ��������
		rq = list_first_entry(&dd->dispatch, struct request, queuelist);
		list_del_init(&rq->queuelist);
		goto done;
	}
    //deadline�����㷨fifo�����в���readsΪ1
	reads = !list_empty(&dd->fifo_list[READ]);
    //deadline�����㷨fifoд���в���writesΪ1
	writes = !list_empty(&dd->fifo_list[WRITE]);

	/*
	 * batches are currently reads XOR writes
	 */
	//ÿ�δӺ����ѡȡһ��req�����������䣬���req����һ��req������next_rq������������������req���䣬�ȴ�next_rqȡ��req
	if (dd->next_rq[WRITE])
		rq = dd->next_rq[WRITE];
	else
		rq = dd->next_rq[READ];

//���dd->batching���ڵ���dd->fifo_batch������ʹ��next_rq�������һֱֻ���ʹ�ú�������е�req���������ʹ��䣬����ǰ�ߵ�req�ò�������
	if (rq && dd->batching < dd->fifo_batch)
		/* we have a next request are still entitled to batch */
		goto dispatch_request;

	/*
	 * at this point we are not running a batch. select the appropriate
	 * data direction (read / write)
	 */
    //��Ӧ����ѡ��ѡ��read��write req����Ϊһֱѡ��read req���������䣬��write req��starve������
	if (reads) {
		BUG_ON(RB_EMPTY_ROOT(&dd->sort_list[READ]));
        //��write reqҪ���͸�����������write req�����������ﵽ���ޣ���ǿ��ѡ����תѡ��write req
        //��ֹһֱѡ��read req���������䣬write req�ò���ѡ���starve������ÿ��write req�ò���ѡ���������starved++��
        //writes_starved�Ǽ����Ĵ������ޣ�starved����writes_starved����ǿ��ѡ��write req
		if (writes && (dd->starved++ >= dd->writes_starved))
			goto dispatch_writes;

         //��������ѡ��read req
		data_dir = READ;

		goto dispatch_find_request;
	}

	/*
	 * there are either no reads or writes have been starved
	 */

	if (writes) {
dispatch_writes:
		BUG_ON(RB_EMPTY_ROOT(&dd->sort_list[WRITE]));
        //dd->starved��0
		dd->starved = 0;
        //����ѡ��write req����һ����ֵ����
		data_dir = WRITE;

		goto dispatch_find_request;
	}

	return NULL;

dispatch_find_request:
	/*
	 * we are not running a batch, find best request for selected data_dir
	 */
	//deadline_check_fifo���deadline fifo�����г�ʱ��reqҪ���䷵��1������next_rqû���ݴ�req���Ǿʹ�fifo����ͷȡ��req
	if (deadline_check_fifo(dd, data_dir) || !dd->next_rq[data_dir]) {
		/*
		 * A deadline has expired, the last request was in the other
		 * direction, or we have run out of higher-sectored requests.
		 * Start again from the request with the earliest expiry time.
		 */
		//ȡ��fifo����ͷ��req��������fifo���е�req��������ӵ�req��Ȼ�����׳�ʱ
		rq = rq_entry_fifo(dd->fifo_list[data_dir].next);
	} else {
		/*
		 * The last req was the same dir and we have a next request in
		 * sort order. No expired requests so continue on from here.
		 */
		//����ֱ��ȡ��next_rq�ݴ��req��data_dir��ǰ�߸�ֵ����д
		rq = dd->next_rq[data_dir];
	}

	dd->batching = 0;

dispatch_request://�������reqֱ������next_rq����fifo���У����req��Ҫ����������������
	/*
	 * rq is the selected appropriate request.
	 */
	//batching��1
	dd->batching++;

    //�Ӻ����������req��next req��dd->next_rq[data_dir],��req��fifo���кͺ���������޳�
	deadline_move_request(dd, rq);
done:
	rq->cmd_flags |= REQ_STARTED;
	return rq;
}

static struct request *dd_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct deadline_data *dd = hctx->queue->elevator->elevator_data;
	struct request *rq;

	spin_lock(&dd->lock);

 //ִ��deadline�㷨�ɷ���������fifo���ߺ��������ѡ����ɵ�req���ء�Ȼ�������µ�next_rq������req��fifo���кͺ���������޳���
 //req��Դ��:�ϴ��ɷ����õ�next_rq;read req�ɷ������ѡ���write req;fifo �����ϳ�ʱҪ�����req��ͳ���ˣ��й̶�����
	rq = __dd_dispatch_request(hctx);
	spin_unlock(&dd->lock);

	return rq;
}

static void dd_exit_queue(struct elevator_queue *e)
{
	struct deadline_data *dd = e->elevator_data;

	BUG_ON(!list_empty(&dd->fifo_list[READ]));
	BUG_ON(!list_empty(&dd->fifo_list[WRITE]));

	kfree(dd);
}

/*
 * initialize elevator private data (deadline_data).
 */
static int dd_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct deadline_data *dd;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	dd = kzalloc_node(sizeof(*dd), GFP_KERNEL, q->node);
	if (!dd) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = dd;

	INIT_LIST_HEAD(&dd->fifo_list[READ]);
	INIT_LIST_HEAD(&dd->fifo_list[WRITE]);
	dd->sort_list[READ] = RB_ROOT;
	dd->sort_list[WRITE] = RB_ROOT;
	dd->fifo_expire[READ] = read_expire;
	dd->fifo_expire[WRITE] = write_expire;
	dd->writes_starved = writes_starved;
	dd->front_merges = 1;
	dd->fifo_batch = fifo_batch;
	spin_lock_init(&dd->lock);
	INIT_LIST_HEAD(&dd->dispatch);

	q->elevator = eq;
	return 0;
}

static int dd_request_merge(struct request_queue *q, struct request **rq,
			    struct bio *bio)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	sector_t sector = bio_end_sector(bio);
	struct request *__rq;

	if (!dd->front_merges)
		return ELEVATOR_NO_MERGE;
    
    //�ú������ڵ����㷨�� ����д��������������req,�ҵ�req��ʼ������ַ����bio_end_sector(bio)��req���أ����򷵻�NULL
	__rq = elv_rb_find(&dd->sort_list[bio_data_dir(bio)], sector);
	if (__rq) {
		BUG_ON(sector != blk_rq_pos(__rq));
    //����ҵ�ƥ���req��˵��bio������������ַ����req��������ʼ��ַ���򷵻�ǰ��ϲ�
		if (elv_bio_merge_ok(__rq, bio)) {
			*rq = __rq;
			return ELEVATOR_FRONT_MERGE;
		}
	}

	return ELEVATOR_NO_MERGE;
}
//��IO����������������Ƿ��п��Ժϲ���req���ҵ������bio�����ǰ��ϲ���req�����ᴥ�����κϲ�������Ժϲ����req��IO�����㷨��������������
//����ϲ���������к�Ӳ������û�а�ëǮ�Ĺ�ϵ��
static bool dd_bio_merge(struct blk_mq_hw_ctx *hctx, struct bio *bio)
{
    //����Ψһ��Ӧ�Ķ���request_queue
	struct request_queue *q = hctx->queue;
	struct deadline_data *dd = q->elevator->elevator_data;
	struct request *free = NULL;
	bool ret;

	spin_lock(&dd->lock);
//��IO����������������Ƿ��п��Ժϲ���req���ҵ������bio�����ǰ��ϲ���req�����ᴥ�����κϲ�������Ժϲ����req��IO�����㷨��������������
	ret = blk_mq_sched_try_merge(q, bio, &free);
	spin_unlock(&dd->lock);

	if (free)
		blk_mq_free_request(free);

	return ret;
}

/*
 * add rq to rbtree and fifo
 */
//���Խ�req�ϲ���q->last_merg���ߵ����㷨��hash���е��ٽ�req���ϲ����˵Ļ�����req���뵽deadline�����㷨�ĺ������fifo���У�����req��fifo
//���еĳ�ʱʱ�䡣������elv�����㷨��hash���С�ע�⣬hash���в���deadline�����㷨���еġ�hash���к�fifo���е����ǲ���һ����???????
static void dd_insert_request(struct blk_mq_hw_ctx *hctx, struct request *rq,
			      bool at_head)//at_head:false
{
	struct request_queue *q = hctx->queue;
	struct deadline_data *dd = q->elevator->elevator_data;
	const int data_dir = rq_data_dir(rq);

    //���ȳ��Խ�rq����ϲ���q->last_merge���ٳ��Խ�rq����ϲ���hash���е�ĳһ��__rq���ϲ�������rq��������ʼ��ַ����q->last_merge��__rq
    //������������ַ�����ǵ���blk_attempt_req_merge()���кϲ���������IOʹ���ʵ����ݡ����ʹ����deadline�����㷨�����ºϲ����req��
    //hash�����е�λ�á������fifo�����޳���rq������dd->next_rq[]��ֵrq����һ��req��
	if (blk_mq_sched_try_insert_merge(q, rq))
		return;

	blk_mq_sched_request_inserted(rq);

	if (at_head || rq->cmd_type != REQ_TYPE_FS) {//no,һ��req����REQ_TYPE_FS
	    //���at_headΪTRUE�����req��ӵ������㷨��dd->dispatch����
		if (at_head)
			list_add(&rq->queuelist, &dd->dispatch);
		else
			list_add_tail(&rq->queuelist, &dd->dispatch);
	} else {
	    //����req�Ĵ�����ʼ��ַ��req��ӵ���������������������req�����й����ǣ�˭�Ĵ�����ʼ��ַС˭����
		deadline_add_rq_rb(dd, rq);

		if (rq_mergeable(rq)) {//yes
            //req��������������ַrq_hash_key(rq)��ӵ�IO�����㷨��hash�������hash������Ϊ����IO�㷨�������������Ժϲ���reqʱ����������ٶ�
			elv_rqhash_add(q, rq);
			if (!q->last_merge)
				q->last_merge = rq;
		}
		/*������Ҿ��ú���֣�deadline�����㷨��������3������ѽ��������struct elevator_queue��struct deadline_data
		  ��struct rb_root sort_list[2]��������к�struct list_head fifo_list[2]fifo���У�����һ��struct elevator_queue
		  ���hash���С�hash������ÿ�������㷨���еģ�fifo�ͺ����������deadline���е�*/
		
		/*
		 * set expire time and add to fifo list
		 */
		//����req��fifo���еĳ�ʱʱ��
		rq->fifo_time = jiffies + dd->fifo_expire[data_dir];
        //��req����fifo������
		list_add_tail(&rq->queuelist, &dd->fifo_list[data_dir]);
	}
}

static void dd_insert_requests(struct blk_mq_hw_ctx *hctx,
			       struct list_head *list, bool at_head)//at_head:false
{
	struct request_queue *q = hctx->queue;
	struct deadline_data *dd = q->elevator->elevator_data;

	spin_lock(&dd->lock);
    //���α�����ǰ����plug->mq_list�����ϵ�req,
	while (!list_empty(list)) {
		struct request *rq;
        //req��ԭ����mq_list������ɾ����
		rq = list_first_entry(list, struct request, queuelist);
		list_del_init(&rq->queuelist);
     //���Խ�req�ϲ���q->last_merg���ߵ����㷨��hash���е��ٽ�req���ϲ����˵Ļ�����req���뵽deadline�����㷨�ĺ������fifo���У�
     //����req��fifo���еĳ�ʱʱ�䡣������elv�����㷨��hash���С�ע�⣬hash���в���deadline�����㷨���еġ�
		dd_insert_request(hctx, rq, at_head);
	}
	spin_unlock(&dd->lock);
}

static bool dd_has_work(struct blk_mq_hw_ctx *hctx)
{
	struct deadline_data *dd = hctx->queue->elevator->elevator_data;

    //��reqҪ����ʱ������1
	return !list_empty_careful(&dd->dispatch) ||
		!list_empty_careful(&dd->fifo_list[0]) ||
		!list_empty_careful(&dd->fifo_list[1]);
}

/*
 * sysfs parts below
 */
static ssize_t
deadline_var_show(int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t
deadline_var_store(int *var, const char *page, size_t count)
{
	char *p = (char *) page;

	*var = simple_strtol(p, &p, 10);
	return count;
}

#define SHOW_FUNCTION(__FUNC, __VAR, __CONV)				\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct deadline_data *dd = e->elevator_data;			\
	int __data = __VAR;						\
	if (__CONV)							\
		__data = jiffies_to_msecs(__data);			\
	return deadline_var_show(__data, (page));			\
}
SHOW_FUNCTION(deadline_read_expire_show, dd->fifo_expire[READ], 1);
SHOW_FUNCTION(deadline_write_expire_show, dd->fifo_expire[WRITE], 1);
SHOW_FUNCTION(deadline_writes_starved_show, dd->writes_starved, 0);
SHOW_FUNCTION(deadline_front_merges_show, dd->front_merges, 0);
SHOW_FUNCTION(deadline_fifo_batch_show, dd->fifo_batch, 0);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count)	\
{									\
	struct deadline_data *dd = e->elevator_data;			\
	int __data;							\
	int ret = deadline_var_store(&__data, (page), count);		\
	if (__data < (MIN))						\
		__data = (MIN);						\
	else if (__data > (MAX))					\
		__data = (MAX);						\
	if (__CONV)							\
		*(__PTR) = msecs_to_jiffies(__data);			\
	else								\
		*(__PTR) = __data;					\
	return ret;							\
}
STORE_FUNCTION(deadline_read_expire_store, &dd->fifo_expire[READ], 0, INT_MAX, 1);
STORE_FUNCTION(deadline_write_expire_store, &dd->fifo_expire[WRITE], 0, INT_MAX, 1);
STORE_FUNCTION(deadline_writes_starved_store, &dd->writes_starved, INT_MIN, INT_MAX, 0);
STORE_FUNCTION(deadline_front_merges_store, &dd->front_merges, 0, 1, 0);
STORE_FUNCTION(deadline_fifo_batch_store, &dd->fifo_batch, 0, INT_MAX, 0);
#undef STORE_FUNCTION

#define DD_ATTR(name) \
	__ATTR(name, S_IRUGO|S_IWUSR, deadline_##name##_show, \
				      deadline_##name##_store)

static struct elv_fs_entry deadline_attrs[] = {
	DD_ATTR(read_expire),
	DD_ATTR(write_expire),
	DD_ATTR(writes_starved),
	DD_ATTR(front_merges),
	DD_ATTR(fifo_batch),
	__ATTR_NULL
};

#ifdef CONFIG_BLK_DEBUG_FS
#define DEADLINE_DEBUGFS_DDIR_ATTRS(ddir, name)				\
static void *deadline_##name##_fifo_start(struct seq_file *m,		\
					  loff_t *pos)			\
	__acquires(&dd->lock)						\
{									\
	struct request_queue *q = m->private;				\
	struct deadline_data *dd = q->elevator->elevator_data;		\
									\
	spin_lock(&dd->lock);						\
	return seq_list_start(&dd->fifo_list[ddir], *pos);		\
}									\
									\
static void *deadline_##name##_fifo_next(struct seq_file *m, void *v,	\
					 loff_t *pos)			\
{									\
	struct request_queue *q = m->private;				\
	struct deadline_data *dd = q->elevator->elevator_data;		\
									\
	return seq_list_next(v, &dd->fifo_list[ddir], pos);		\
}									\
									\
static void deadline_##name##_fifo_stop(struct seq_file *m, void *v)	\
	__releases(&dd->lock)						\
{									\
	struct request_queue *q = m->private;				\
	struct deadline_data *dd = q->elevator->elevator_data;		\
									\
	spin_unlock(&dd->lock);						\
}									\
									\
static const struct seq_operations deadline_##name##_fifo_seq_ops = {	\
	.start	= deadline_##name##_fifo_start,				\
	.next	= deadline_##name##_fifo_next,				\
	.stop	= deadline_##name##_fifo_stop,				\
	.show	= blk_mq_debugfs_rq_show,				\
};									\
									\
static int deadline_##name##_next_rq_show(void *data,			\
					  struct seq_file *m)		\
{									\
	struct request_queue *q = data;					\
	struct deadline_data *dd = q->elevator->elevator_data;		\
	struct request *rq = dd->next_rq[ddir];				\
									\
	if (rq)								\
		__blk_mq_debugfs_rq_show(m, rq);			\
	return 0;							\
}
DEADLINE_DEBUGFS_DDIR_ATTRS(READ, read)
DEADLINE_DEBUGFS_DDIR_ATTRS(WRITE, write)
#undef DEADLINE_DEBUGFS_DDIR_ATTRS

static int deadline_batching_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data;
	struct deadline_data *dd = q->elevator->elevator_data;

	seq_printf(m, "%u\n", dd->batching);
	return 0;
}

static int deadline_starved_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data;
	struct deadline_data *dd = q->elevator->elevator_data;

	seq_printf(m, "%u\n", dd->starved);
	return 0;
}

static void *deadline_dispatch_start(struct seq_file *m, loff_t *pos)
	__acquires(&dd->lock)
{
	struct request_queue *q = m->private;
	struct deadline_data *dd = q->elevator->elevator_data;

	spin_lock(&dd->lock);
	return seq_list_start(&dd->dispatch, *pos);
}

static void *deadline_dispatch_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct request_queue *q = m->private;
	struct deadline_data *dd = q->elevator->elevator_data;

	return seq_list_next(v, &dd->dispatch, pos);
}

static void deadline_dispatch_stop(struct seq_file *m, void *v)
	__releases(&dd->lock)
{
	struct request_queue *q = m->private;
	struct deadline_data *dd = q->elevator->elevator_data;

	spin_unlock(&dd->lock);
}

static const struct seq_operations deadline_dispatch_seq_ops = {
	.start	= deadline_dispatch_start,
	.next	= deadline_dispatch_next,
	.stop	= deadline_dispatch_stop,
	.show	= blk_mq_debugfs_rq_show,
};

#define DEADLINE_QUEUE_DDIR_ATTRS(name)						\
	{#name "_fifo_list", 0400, .seq_ops = &deadline_##name##_fifo_seq_ops},	\
	{#name "_next_rq", 0400, deadline_##name##_next_rq_show}
static const struct blk_mq_debugfs_attr deadline_queue_debugfs_attrs[] = {
	DEADLINE_QUEUE_DDIR_ATTRS(read),
	DEADLINE_QUEUE_DDIR_ATTRS(write),
	{"batching", 0400, deadline_batching_show},
	{"starved", 0400, deadline_starved_show},
	{"dispatch", 0400, .seq_ops = &deadline_dispatch_seq_ops},
	{},
};
#undef DEADLINE_QUEUE_DDIR_ATTRS
#endif

static struct elevator_mq_ops dd_ops = {
	.insert_requests	= dd_insert_requests,
	.dispatch_request	= dd_dispatch_request,
	.next_request		= elv_rb_latter_request,
	.former_request		= elv_rb_former_request,
	.bio_merge		= dd_bio_merge,//�ϲ�
	.request_merge		= dd_request_merge,
	.requests_merged	= dd_merged_requests,
	.request_merged		= dd_request_merged,
	.has_work		= dd_has_work,
	.init_sched		= dd_init_queue,
	.exit_sched		= dd_exit_queue,
};

static struct elevator_type mq_deadline = {
	.elevator_attrs = deadline_attrs,
	.elevator_name = "mq-deadline",
	.elevator_owner = THIS_MODULE,
};
MODULE_ALIAS("mq-deadline-iosched");

static int __init deadline_init(void)
{
	int ret = elv_register(&mq_deadline);
	struct elevator_type_aux *aux;

	if (ret)
		return ret;

	aux = elevator_aux_find(&mq_deadline);
	memcpy(&aux->ops.mq, &dd_ops, sizeof(struct elevator_mq_ops));
	aux->uses_mq = true;
	aux->elevator_alias = "deadline",
	aux->queue_debugfs_attrs = deadline_queue_debugfs_attrs;

	return 0;
}

static void __exit deadline_exit(void)
{
	elv_unregister(&mq_deadline);
}

module_init(deadline_init);
module_exit(deadline_exit);

MODULE_AUTHOR("Jens Axboe");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MQ deadline IO scheduler");
