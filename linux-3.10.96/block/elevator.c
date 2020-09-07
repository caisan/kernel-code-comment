/*
 *  Block device elevator/IO-scheduler.
 *
 *  Copyright (C) 2000 Andrea Arcangeli <andrea@suse.de> SuSE
 *
 * 30042000 Jens Axboe <axboe@kernel.dk> :
 *
 * Split the elevator a bit so that it is possible to choose a different
 * one or even write a new "plug in". There are three pieces:
 * - elevator_fn, inserts a new request in the queue list
 * - elevator_merge_fn, decides whether a new buffer can be merged with
 *   an existing request
 * - elevator_dequeue_fn, called when a request is taken off the active list
 *
 * 20082000 Dave Jones <davej@suse.de> :
 * Removed tests for max-bomb-segments, which was breaking elvtune
 *  when run without -bN
 *
 * Jens:
 * - Rework again to work with bio instead of buffer_heads
 * - loose bi_dev comparisons, partition handling is right now
 * - completely modularize elevator setup and teardown
 *
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/blktrace_api.h>
#include <linux/hash.h>
#include <linux/uaccess.h>
#include <linux/pm_runtime.h>

#include <trace/events/block.h>

#include "blk.h"
#include "blk-cgroup.h"

static DEFINE_SPINLOCK(elv_list_lock);
static LIST_HEAD(elv_list);

/*
 * Merge hash stuff.
 */
//hash key��req������������ַѽ
#define rq_hash_key(rq)		(blk_rq_pos(rq) + blk_rq_sectors(rq))

/*
 * Query io scheduler to see if the current process issuing bio may be
 * merged with rq.
 */
static int elv_iosched_allow_merge(struct request *rq, struct bio *bio)
{
	struct request_queue *q = rq->q;
	struct elevator_queue *e = q->elevator;

	if (e->type->ops.elevator_allow_merge_fn)
		return e->type->ops.elevator_allow_merge_fn(q, rq, bio);

	return 1;
}

/*
 * can we safely merge with this request?
 */
bool elv_rq_merge_ok(struct request *rq, struct bio *bio)
{
    //�Ա����µ�bio�ܷ�ϲ���rq�������е�rq������ǰ�ڼ�飬���ͨ������true
	if (!blk_rq_merge_ok(rq, bio))
		return 0;

	if (!elv_iosched_allow_merge(rq, bio))
		return 0;

	return 1;
}
EXPORT_SYMBOL(elv_rq_merge_ok);

static struct elevator_type *elevator_find(const char *name)
{
	struct elevator_type *e;

	list_for_each_entry(e, &elv_list, list) {
		if (!strcmp(e->elevator_name, name))
			return e;
	}

	return NULL;
}

static void elevator_put(struct elevator_type *e)
{
	module_put(e->elevator_owner);
}

static struct elevator_type *elevator_get(const char *name, bool try_loading)
{
	struct elevator_type *e;

	spin_lock(&elv_list_lock);

	e = elevator_find(name);
	if (!e && try_loading) {
		spin_unlock(&elv_list_lock);
		request_module("%s-iosched", name);
		spin_lock(&elv_list_lock);
		e = elevator_find(name);
	}

	if (e && !try_module_get(e->elevator_owner))
		e = NULL;

	spin_unlock(&elv_list_lock);

	return e;
}

static char chosen_elevator[ELV_NAME_MAX];

static int __init elevator_setup(char *str)
{
	/*
	 * Be backwards-compatible with previous kernels, so users
	 * won't get the wrong elevator.
	 */
	strncpy(chosen_elevator, str, sizeof(chosen_elevator) - 1);
	return 1;
}

__setup("elevator=", elevator_setup);

/* called during boot to load the elevator chosen by the elevator param */
void __init load_default_elevator_module(void)
{
	struct elevator_type *e;

	if (!chosen_elevator[0])
		return;

	spin_lock(&elv_list_lock);
	e = elevator_find(chosen_elevator);
	spin_unlock(&elv_list_lock);

	if (!e)
		request_module("%s-iosched", chosen_elevator);
}

static struct kobj_type elv_ktype;

struct elevator_queue *elevator_alloc(struct request_queue *q,
				  struct elevator_type *e)
{
	struct elevator_queue *eq;

	eq = kmalloc_node(sizeof(*eq), GFP_KERNEL | __GFP_ZERO, q->node);
	if (unlikely(!eq))
		goto err;

	eq->type = e;
	kobject_init(&eq->kobj, &elv_ktype);
	mutex_init(&eq->sysfs_lock);
	hash_init(eq->hash);

	return eq;
err:
	kfree(eq);
	elevator_put(e);
	return NULL;
}
EXPORT_SYMBOL(elevator_alloc);

static void elevator_release(struct kobject *kobj)
{
	struct elevator_queue *e;

	e = container_of(kobj, struct elevator_queue, kobj);
	elevator_put(e->type);
	kfree(e);
}

int elevator_init(struct request_queue *q, char *name)
{
	struct elevator_type *e = NULL;
	int err;

	/*
	 * q->sysfs_lock must be held to provide mutual exclusion between
	 * elevator_switch() and here.
	 */
	lockdep_assert_held(&q->sysfs_lock);

	if (unlikely(q->elevator))
		return 0;

	INIT_LIST_HEAD(&q->queue_head);
	q->last_merge = NULL;
	q->end_sector = 0;
	q->boundary_rq = NULL;

	if (name) {
		e = elevator_get(name, true);
		if (!e)
			return -EINVAL;
	}

	/*
	 * Use the default elevator specified by config boot param or
	 * config option.  Don't try to load modules as we could be running
	 * off async and request_module() isn't allowed from async.
	 */
	if (!e && *chosen_elevator) {
		e = elevator_get(chosen_elevator, false);
		if (!e)
			printk(KERN_ERR "I/O scheduler %s not found\n",
							chosen_elevator);
	}

	if (!e) {
		e = elevator_get(CONFIG_DEFAULT_IOSCHED, false);
		if (!e) {
			printk(KERN_ERR
				"Default I/O scheduler not found. " \
				"Using noop.\n");
			e = elevator_get("noop", false);
		}
	}

	err = e->ops.elevator_init_fn(q, e);
	return 0;
}
EXPORT_SYMBOL(elevator_init);

void elevator_exit(struct elevator_queue *e)
{
	mutex_lock(&e->sysfs_lock);
	if (e->type->ops.elevator_exit_fn)
		e->type->ops.elevator_exit_fn(e);
	mutex_unlock(&e->sysfs_lock);

	kobject_put(&e->kobj);
}
EXPORT_SYMBOL(elevator_exit);

static inline void __elv_rqhash_del(struct request *rq)
{
	hash_del(&rq->hash);
}

static void elv_rqhash_del(struct request_queue *q, struct request *rq)
{
	if (ELV_ON_HASH(rq))
		__elv_rqhash_del(rq);
}

static void elv_rqhash_add(struct request_queue *q, struct request *rq)
{
    //e����IO�����㷨ʵ��
	struct elevator_queue *e = q->elevator;

	BUG_ON(ELV_ON_HASH(rq));
    //req��������������ַrq_hash_key(rq)��ӵ�IO�����㷨��hash�������hash������Ϊ����IO�㷨�������������Ժϲ���reqʱ����������ٶ�
	hash_add(e->hash, &rq->hash, rq_hash_key(rq));
}

static void elv_rqhash_reposition(struct request_queue *q, struct request *rq)
{
    //ɾ��req
	__elv_rqhash_del(rq);
    //�����req��Ӧ����req�����µĺϲ�������Ҫ��������req��
	elv_rqhash_add(q, rq);
}
//�¼���IO���ȶ��е�req����hash��������Ӧ�����Ǹ���bio��������ʼ��ַ��hash����req�ɣ�Ȼ��rq_hash_key(rq)��offset��Ⱦ����ҵ���rq
//����ҵ�ƥ���req����req������������ַ����bio��������ʼ��ַ������bio���Ǻ���ϲ���req
static struct request *elv_rqhash_find(struct request_queue *q, sector_t offset)//offset��bio->bi_sector��
{
	struct elevator_queue *e = q->elevator;
	struct hlist_node *next;
	struct request *rq;

    //�µ�req�����hash��ӵ�IO�����㷨��hash����e->hash���hash������Ϊ����IO�㷨�������������Ժϲ���reqʱ����������ٶ�
    //�������hash�����ϵ�req
	hash_for_each_possible_safe(e->hash, rq, next, hash, offset) {
		BUG_ON(!ELV_ON_HASH(rq));

		if (unlikely(!rq_mergeable(rq))) {
			__elv_rqhash_del(rq);
			continue;
		}
        //req������������ַ����offset��offset��bio��������ʼ��ַ(�ú���������offset=blk_rq_pos(rq))������bio���Ǻ���ϲ���req
        //hash key��req������������ַ
		if (rq_hash_key(rq) == offset)
			return rq;
	}

	return NULL;
}

/*
 * RB-tree support functions for inserting/lookup/removal of requests
 * in a sorted RB tree.
 */
//����req�Ĵ�����ʼ��ַ��req��ӵ���������������������req�����й����ǣ�˭�Ĵ�����ʼ��ַС˭����
void elv_rb_add(struct rb_root *root, struct request *rq)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct request *__rq;

	while (*p) {
		parent = *p;
        //ȡ��������е�__rq
		__rq = rb_entry(parent, struct request, rb_node);
        //rq�Ĵ�����ʼ��ַ��__rq��С����rqӦ�ò��뵽����������Ȼ��˭�Ĵ�����ʼ��ַС��˭����
		if (blk_rq_pos(rq) < blk_rq_pos(__rq))
			p = &(*p)->rb_left;
        //����rq�Ĵ�����ʼ��ַ>=__rq�ģ���rqӦ�ò��뵽������
		else if (blk_rq_pos(rq) >= blk_rq_pos(__rq))
			p = &(*p)->rb_right;
	}
    //rq���ӵ������
	rb_link_node(&rq->rb_node, parent, p);
	rb_insert_color(&rq->rb_node, root);
}
EXPORT_SYMBOL(elv_rb_add);

void elv_rb_del(struct rb_root *root, struct request *rq)
{
	BUG_ON(RB_EMPTY_NODE(&rq->rb_node));
	rb_erase(&rq->rb_node, root);
	RB_CLEAR_NODE(&rq->rb_node);
}
EXPORT_SYMBOL(elv_rb_del);


//�ú������ڵ����㷨�� ����д��������������req,�ҵ�req��ʼ������ַ����bio_end_sector(bio)��req���أ����򷵻�NULL
//struct rb_root *root���ȶ��б���req�ĺ����ͷ���ɣ���������һ������һ��д������������е�req����Ĺ�����req�Ĵ�����ʼ������
//������Ϊ�ǰ��մ�����ʼ������С�������еİɡ�sector��bio_end_sector(bio)��bio����Ĵ��������Ľ�����ַ��
struct request *elv_rb_find(struct rb_root *root, sector_t sector)
{
	struct rb_node *n = root->rb_node;
	struct request *rq;

	while (n) {
        //��root���ڵ㿪ʼ����������ȡ��������е�һ��req
		rq = rb_entry(n, struct request, rb_node);
        //bio������������ַsector С�� req����ʼ������ַ��������������������ԣ���������е�req���й����ǣ�˭����ʼ������ַС˭����
		if (sector < blk_rq_pos(rq))
			n = n->rb_left;
        //bio������������ַsector ����  req����ʼ������ַ�������������
		else if (sector > blk_rq_pos(rq))
			n = n->rb_right;
		else//��ȥ������bio������������ַsector = req����ʼ������ַѽ����ʾҪ��bio�ϲ���reqǰ�ߣ�����ǰ��ϲ�
			return rq;
	}
    //����ߵ����Ӧ���Ǻ�������о�û��req��Ա�ɣ��ǿյģ���ΪֻҪ�������һ��req,��bio�Ĵ��̽�����ַ���϶� ���ڻ�С�ڻ���� req����ʼ��ַ��
    //���˴��ˣ�ֻ��sector = blk_rq_pos(rq)�Ż�return rq�����û���ҵ�ƥ���req,n = n->rb_right/rb_left���������ף�nΪNULL�˳�ѭ��
	return NULL;
}
EXPORT_SYMBOL(elv_rb_find);

/*
 * Insert rq into dispatch queue of q.  Queue lock must be held on
 * entry.  rq is sort instead into the dispatch queue. To be used by
 * specific elevators.
 */
void elv_dispatch_sort(struct request_queue *q, struct request *rq)
{
	sector_t boundary;
	struct list_head *entry;
	int stop_flags;

	if (q->last_merge == rq)
		q->last_merge = NULL;

	elv_rqhash_del(q, rq);

	q->nr_sorted--;

	boundary = q->end_sector;
	stop_flags = REQ_SOFTBARRIER | REQ_STARTED;
	list_for_each_prev(entry, &q->queue_head) {
		struct request *pos = list_entry_rq(entry);

		if ((rq->cmd_flags & REQ_DISCARD) !=
		    (pos->cmd_flags & REQ_DISCARD))
			break;
		if (rq_data_dir(rq) != rq_data_dir(pos))
			break;
		if (pos->cmd_flags & stop_flags)
			break;
		if (blk_rq_pos(rq) >= boundary) {
			if (blk_rq_pos(pos) < boundary)
				continue;
		} else {
			if (blk_rq_pos(pos) >= boundary)
				break;
		}
		if (blk_rq_pos(rq) >= blk_rq_pos(pos))
			break;
	}

	list_add(&rq->queuelist, entry);
}
EXPORT_SYMBOL(elv_dispatch_sort);

/*
 * Insert rq into dispatch queue of q.  Queue lock must be held on
 * entry.  rq is added to the back of the dispatch queue. To be used by
 * specific elevators.
 */
//��req��ӵ�rq��queue_head���У�������������������Ǵ�queue_head����ȡ��req�����
void elv_dispatch_add_tail(struct request_queue *q, struct request *rq)
{
	if (q->last_merge == rq)
		q->last_merge = NULL;

	elv_rqhash_del(q, rq);
    //
	q->nr_sorted--;
    //��������
	q->end_sector = rq_end_sector(rq);
	q->boundary_rq = rq;
    //��req��ӵ�rq��queue_head���У�������������������Ǵ�queue_head����ȡ��req�����
	list_add_tail(&rq->queuelist, &q->queue_head);
}
EXPORT_SYMBOL(elv_dispatch_add_tail);

//��elv������������Ƿ��п��Ժϲ���req���ҵ������bio�����ǰ��ϲ���req������ǵ��þ����IO�����㷨����Ѱ�ҿ��Ժϲ���req��
//��������ֵ ELEVATOR_BACK_MERGE(ǰ��ϲ���req)��ELEVATOR_FRONT_MERGE(ǰ��ϲ�)��ELEVATOR_NO_MERGE(û���ҵ����Ժϲ���req)
int elv_merge(struct request_queue *q, struct request **req, struct bio *bio)
{
	struct elevator_queue *e = q->elevator;
	struct request *__rq;
	int ret;

	/*
	 * Levels of merges:
	 * 	nomerges:  No merges at all attempted
	 * 	noxmerges: Only simple one-hit cache try
	 * 	merges:	   All merge tries attempted
	 */
	if (blk_queue_nomerges(q))
		return ELEVATOR_NO_MERGE;

	/*
	 * First try one-hit cache.
	 */
	//�Ƿ���԰�bio�ϲ���q->last_merge���ϴ�rq���кϲ�����rq��elv_rq_merge_ok����һЩȨ�޼��ɶ��
	if (q->last_merge && elv_rq_merge_ok(q->last_merge, bio)) {
        //���bio��rq����Ĵ��̷�Χ�Ƿ��ţ���������Ժϲ�bio��q->last_merge
		ret = blk_try_merge(q->last_merge, bio);
		if (ret != ELEVATOR_NO_MERGE) {
			*req = q->last_merge;
			return ret;
		}
	}

	if (blk_queue_noxmerges(q))
		return ELEVATOR_NO_MERGE;

	/*
	 * See if our hash lookup can find a potential backmerge.
	 */
	 //�¼���IO���ȶ��е�req����hash��������Ӧ�����Ǹ���bio��������ʼ��ַ��hash����req�ɣ�Ȼ��rq_hash_key(rq)��offset��Ⱦ����ҵ���rq
	 //����ҵ�ƥ���req����req������������ַ����bio��������ʼ��ַ������bio���Ǻ���ϲ���req
	__rq = elv_rqhash_find(q, bio->bi_sector);
	if (__rq && elv_rq_merge_ok(__rq, bio)) {
		*req = __rq;
		return ELEVATOR_BACK_MERGE;//����ELEVATOR_BACK_MERGE�����Ǻ���ϲ�
	}
/*    -----ԭʼ�ں˵ģ��±ߵ���3.10.0.957.27�ںˣ���������e->aux->ops.mq.request_merge
    //����IO�����㷨����cfq_merge����deadline_merge���ҵ����Ժϲ���bio��req???????,���ﷵ��ELEVATOR_FRONT_MERGE��ǰ��ϲ�
	if (e->type->ops.elevator_merge_fn)
		return e->type->ops.elevator_merge_fn(q, req, bio);
*/
    
    if (e->uses_mq && e->aux->ops.mq.request_merge)
       //dd_request_merge �� deadline_merge�ĺ���Դ�����һ���ģ������ڵ����㷨�� ����д�����������ҵ�����bio_end_sector(bio)��req
       //�ҵ�˵��bio������������ַ����req��������ʼ��ַ���򷵻�ǰ��ϲ�ELEVATOR_FRONT_MERGE
          return e->aux->ops.mq.request_merge(q, req, bio);//dd_request_merge
    else if (!e->uses_mq && e->aux->ops.sq.elevator_merge_fn)
    //����IO�����㷨����cfq_merge����deadline_merge���ú������ڵ����㷨�� ����д��������������req,�ҵ�req��ʼ������ַ
    //����bio_end_sector(bio)��req������ҵ�ƥ���req��˵��bio������������ַ����req��������ʼ��ַ���򷵻�ǰ��ϲ�ELEVATOR_FRONT_MERGE
          return e->aux->ops.sq.elevator_merge_fn(q, req, bio);
    
	return ELEVATOR_NO_MERGE;
}

/*
 * Attempt to do an insertion back merge. Only check for the case where
 * we can append 'rq' to an existing request, so we can throw 'rq' away
 * afterwards.
 *
 * Returns true if we merged, false otherwise
 */
//���ȳ��Խ�rq����ϲ���q->last_merge���ٳ��Խ�rq����ϲ���hash���е�ĳһ��__rq���ϲ�������rq��������ʼ��ַ����q->last_merge��__rq
//������������ַ�����ǵ���blk_attempt_req_merge()���кϲ���������IOʹ���ʵ����ݡ����ʹ����deadline�����㷨�����ºϲ����req��
//hash�����е�λ�á������fifo�����޳���rq������dd->next_rq[]��ֵrq����һ��req��
static bool elv_attempt_insert_merge(struct request_queue *q,
				     struct request *rq)
{
	struct request *__rq;
	bool ret;

	if (blk_queue_nomerges(q))
		return false;

	/*
	 * First try one-hit cache.
	 */
//���԰�rq�ϲ���q->last_merge��ߣ�������IOʹ�������ݡ�Ȼ�����IO�����㷨��elevator_merge_req_fn�ص���������Ϊmq-deadline�����㷨ʱ��ִ�й�����:
//rq�Ѿ��ϲ�����q->last_merge��,��fifo�������q->last_merge�ƶ���rq�ڵ��λ�ã�����q->last_merge�ĳ�ʱʱ�䡣��fifo���кͺ�����޳�rq,
//������dd->next_rq[]��ֵrq����һ��req����Ϊq->last_merge�ϲ���rq������������ַ����ˣ���q->last_merge��hash������ɾ���������°�������������ַ��hash����������
	if (q->last_merge && blk_attempt_req_merge(q, q->last_merge, rq))
		return true;

	if (blk_queue_noxmerges(q))
		return false;

	ret = false;
	/*
	 * See if our hash lookup can find a potential backmerge.
	 */
	while (1) {
     //�¼���IO���ȶ��е�req����hash��������Ӧ�����Ǹ���rq��������ʼ��ַ��hash����__rq�ɣ�Ȼ��rq_hash_key(__rq)��offset��Ⱦ����ҵ���req��
     //rq_hash_key(rq)������������ַ������˵����rq��������ʼ��ַ��hash������__rq������������ȣ���Ȼ���а�rq����ϲ���__rq
		__rq = elv_rqhash_find(q, blk_rq_pos(rq));
        //�ٴ�ִ��blk_attempt_req_merge��rq����ϲ���__rq
		if (!__rq || !blk_attempt_req_merge(q, __rq, rq))
			break;

		/* The merged request could be merged with others, try again */
		ret = true;
		rq = __rq;
	}

	return ret;
}
//����ûɶ��req�����µĺϲ������req��IO�����㷨��������������
void elv_merged_request(struct request_queue *q, struct request *rq, int type)
{
	struct elevator_queue *e = q->elevator;

	/*ò��deadline�����㷨�ĺ�������ڲ���reqʱ�����ǰ���req�����������ʼ��ַ���Աȣ�˭��������ʼ��ַС��˭���п���.
	  �� deadline_add_request->deadline_add_rq_rb->elv_rb_add����req��elv_merge->deadline_merge->elv_rb_find����req��
	  blk_mq_sched_try_merge->elv_merged_request->deadline_merged_request����req�������deadline�����㷨�ĺ�������У�
	  ��ǰ��ϲ����req��������������Ϊǰ��ϲ����req������ʼ��ַ��С�ˣ���Ȼ��������ж�req���������˭��������ʼ��ַС˭����,
	  �Ǿ�Ҫ�����req�����ٺ��������������
	  
	  ͬ���ģ�deadline�����㷨��hash���У�Ҳ��һ��req���У������������req������������ַ��Ϊʲô��ô˵��
	  ��hash���ʱ��elv_rqhash_add�������hash_add(e->hash, &rq->hash, rq_hash_key(rq))��rq_hash_key(rq)����hash key��req����������ַ��
	  ������elv_merged_request->elv_rqhash_reposition�У���req�����˺���ϲ�������������ַ����ˣ��Ǿ�Ҫ�����req������hash���г�ϴ����
	  blk_queue_bio->add_acct_request->__elv_add_request->elv_rqhash_add��ӣ�elv_merge->elv_rqhash_find����
	  blk_mq_sched_try_merge->elv_merged_request->elv_rqhash_reposition�������򡣶Ժ���ϲ����req����һ����������
	*/

    //������ǰ��ϲ���IO�����㷨����
	if (e->type->ops.elevator_merged_fn)//cfq_merged_request��deadline_merged_request��mqû��
		e->type->ops.elevator_merged_fn(q, rq, type);

	if (type == ELEVATOR_BACK_MERGE)
		elv_rqhash_reposition(q, rq);//req�����µĺϲ�����req��������

	q->last_merge = rq;
}
//�����next�Ѿ��ϲ�����rq,��fifo�������req�ƶ���next�ڵ��λ�ã�����req�ĳ�ʱʱ�䡣��fifo���кͺ�����޳�next,
//������dd->next_rq[]��ֵnext����һ��req����Ϊrq�ϲ���next������������ַ����ˣ���rq��hash������ɾ������������hash������
void elv_merge_requests(struct request_queue *q, struct request *rq,
			     struct request *next)
{
	struct elevator_queue *e = q->elevator;
	const int next_sorted = next->cmd_flags & REQ_SORTED;



    //��fifo�������req�ƶ���next�ڵ��λ�ã�����req�ĳ�ʱʱ�䡣��fifo���кͺ�����޳�next,������dd->next_rq[]��ֵnext����һ��req
	if (next_sorted && e->type->ops.elevator_merge_req_fn)
		e->type->ops.elevator_merge_req_fn(q, rq, next);//deadline_merged_requests ��noop_merged_requests��cfq_merged_requests

    //3.10.0.957.27�ں������ģ������Ե�mq-deadline��
    //��fifo�������req�ƶ���next�ڵ��λ�ã�����req�ĳ�ʱʱ�䡣��fifo���кͺ�����޳�next,������dd->next_rq[]��ֵnext����һ��req
    if (e->uses_mq && e->aux->ops.mq.requests_merged)
		e->aux->ops.mq.requests_merged(q, rq, next);//dd_merged_requests������ԭ���deadline_merged_requests�����ܽӽ�
		

    //ò���ǰ�rqɾ����������ӵ�������hash����
	elv_rqhash_reposition(q, rq);

	if (next_sorted) {
        //ɾ��next
		elv_rqhash_del(q, next);
		q->nr_sorted--;
	}

	q->last_merge = rq;
}

void elv_bio_merged(struct request_queue *q, struct request *rq,
			struct bio *bio)
{
	struct elevator_queue *e = q->elevator;
    //ֻ������һ����ͳ�����ݰ� 
	if (e->type->ops.elevator_bio_merged_fn)
		e->type->ops.elevator_bio_merged_fn(q, rq, bio);
}

#ifdef CONFIG_PM_RUNTIME
static void blk_pm_requeue_request(struct request *rq)
{
	if (rq->q->dev && !(rq->cmd_flags & REQ_PM))
		rq->q->nr_pending--;
}

static void blk_pm_add_request(struct request_queue *q, struct request *rq)
{
	if (q->dev && !(rq->cmd_flags & REQ_PM) && q->nr_pending++ == 0 &&
	    (q->rpm_status == RPM_SUSPENDED || q->rpm_status == RPM_SUSPENDING))
		pm_request_resume(q->dev);
}
#else
static inline void blk_pm_requeue_request(struct request *rq) {}
static inline void blk_pm_add_request(struct request_queue *q,
				      struct request *rq)
{
}
#endif

void elv_requeue_request(struct request_queue *q, struct request *rq)
{
	/*
	 * it already went through dequeue, we need to decrement the
	 * in_flight count again
	 */
	if (blk_account_rq(rq)) {
		q->in_flight[rq_is_sync(rq)]--;
		if (rq->cmd_flags & REQ_SORTED)
			elv_deactivate_rq(q, rq);
	}

	rq->cmd_flags &= ~REQ_STARTED;

	blk_pm_requeue_request(rq);

	__elv_add_request(q, rq, ELEVATOR_INSERT_REQUEUE);
}

void elv_drain_elevator(struct request_queue *q)
{
	static int printed;

	lockdep_assert_held(q->queue_lock);
//ѡ�ĺ��ʴ��ɷ������������req,Ȼ���req��ӵ�rq��queue_head���У������µ�next_rq������req��fifo���кͺ���������޳���������������������Ǵ�queue_head����ȡ��req�����
//������ʵ�req����Դ��:�ϴ��ɷ����õ�next_rq;read req�ɷ������ѡ���write req;fifo �����ϳ�ʱҪ�����req��ͬ�ּ�ˣ��й̶�����
	while (q->elevator->type->ops.elevator_dispatch_fn(q, 1))//deadline_dispatch_requests
		;
	if (q->nr_sorted && printed++ < 10) {
		printk(KERN_ERR "%s: forced dispatching is broken "
		       "(nr_sorted=%u), please report this\n",
		       q->elevator->type->elevator_name, q->nr_sorted);
	}
}
//�·����req����IO�㷨���У������ǰѵ�ǰ����plug������reqȫ�����뵽IO�����㷨����
void __elv_add_request(struct request_queue *q, struct request *rq, int where)//whereĬ����ELEVATOR_INSERT_SORT
{
	trace_block_rq_insert(q, rq);

	blk_pm_add_request(q, rq);

	rq->q = q;

	if (rq->cmd_flags & REQ_SOFTBARRIER) {
		/* barriers are scheduling boundary, update end_sector */
		if (rq->cmd_type == REQ_TYPE_FS) {
			q->end_sector = rq_end_sector(rq);
			q->boundary_rq = rq;
		}
	} else if (!(rq->cmd_flags & REQ_ELVPRIV) &&
		    (where == ELEVATOR_INSERT_SORT ||
		     where == ELEVATOR_INSERT_SORT_MERGE))
		where = ELEVATOR_INSERT_BACK;

	switch (where) {
	case ELEVATOR_INSERT_REQUEUE:
	case ELEVATOR_INSERT_FRONT://ǰ��ϲ�
		rq->cmd_flags |= REQ_SOFTBARRIER;
		list_add(&rq->queuelist, &q->queue_head);//req����q->queue_head
		break;

	case ELEVATOR_INSERT_BACK://����ϲ�
		rq->cmd_flags |= REQ_SOFTBARRIER;
        //ѡ�ĺ��ʴ��ɷ������������req,Ȼ���req��ӵ�rq��queue_head���У������µ�next_rq������req��fifo���кͺ���������޳���������������������Ǵ�queue_head����ȡ��req�����
        //������ʵ�req����Դ��:�ϴ��ɷ����õ�next_rq;read req�ɷ������ѡ���write req;fifo �����ϳ�ʱҪ�����req��ͬ�ּ�ˣ��й̶�����
		elv_drain_elevator(q);
		list_add_tail(&rq->queuelist, &q->queue_head);
		/*
		 * We kick the queue here for the following reasons.
		 * - The elevator might have returned NULL previously
		 *   to delay requests and returned them now.  As the
		 *   queue wasn't empty before this request, ll_rw_blk
		 *   won't run the queue on return, resulting in hang.
		 * - Usually, back inserted requests won't be merged
		 *   with anything.  There's no point in delaying queue
		 *   processing.
		 */
		//������õײ��������ݴ��亯�����ͻ��rq��queue_head����ȡ��req���͸���������ȥ����
		__blk_run_queue(q);
		break;

	case ELEVATOR_INSERT_SORT_MERGE://�ѽ��̶��е�plug�����ϵ�req����IO�����㷨������������
		/*
		 * If we succeed in merging this request with one in the
		 * queue already, we are done - rq has now been freed,
		 * so no need to do anything further.
		 */
		if (elv_attempt_insert_merge(q, rq))
			break;
	case ELEVATOR_INSERT_SORT://�·����req�����IO�����㷨����������
		BUG_ON(rq->cmd_type != REQ_TYPE_FS);
		rq->cmd_flags |= REQ_SORTED;
        //���в����µ�һ��req
		q->nr_sorted++;
		if (rq_mergeable(rq)) {
            //�µ�req��rq->hash��ӵ�IO�����㷨��hash������
			elv_rqhash_add(q, rq);
			if (!q->last_merge)
				q->last_merge = rq;
		}

		/*
		 * Some ioscheds (cfq) run q->request_fn directly, so
		 * rq cannot be accessed after calling
		 * elevator_add_req_fn.
		 */
		//��req���뵽IO�����㷨�����deadline�ǲ��뵽��������к�fifo����
		//mq-deadlineû���������,����û��IO�����㷨�ĳ�����ǿ��ִ������������ǻ�����?���ƾͲ�����case ELEVATOR_INSERT_SORT��֧!!!!!
		q->elevator->type->ops.elevator_add_req_fn(q, rq);//deadline_add_request
		break;

	case ELEVATOR_INSERT_FLUSH:
		rq->cmd_flags |= REQ_SOFTBARRIER;
		blk_insert_flush(rq);
		break;
	default:
		printk(KERN_ERR "%s: bad insertion point %d\n",
		       __func__, where);
		BUG();
	}
}
EXPORT_SYMBOL(__elv_add_request);

void elv_add_request(struct request_queue *q, struct request *rq, int where)
{
	unsigned long flags;

	spin_lock_irqsave(q->queue_lock, flags);
	__elv_add_request(q, rq, where);
	spin_unlock_irqrestore(q->queue_lock, flags);
}
EXPORT_SYMBOL(elv_add_request);
//ֻ�Ǵ�IO�����㷨������ȡ��rq����һ��rq��,��ͬ�ĵ����㷨���ȶ��в�һ��
struct request *elv_latter_request(struct request_queue *q, struct request *rq)
{
	struct elevator_queue *e = q->elevator;

	if (e->type->ops.elevator_latter_req_fn)//elv_rb_latter_request��noop_latter_request
		return e->type->ops.elevator_latter_req_fn(q, rq);
	return NULL;
}

struct request *elv_former_request(struct request_queue *q, struct request *rq)
{
	struct elevator_queue *e = q->elevator;
    //elv_rb_former_request��noop_former_request
	if (e->type->ops.elevator_former_req_fn)
		return e->type->ops.elevator_former_req_fn(q, rq);
	return NULL;
}

int elv_set_request(struct request_queue *q, struct request *rq,
		    struct bio *bio, gfp_t gfp_mask)
{
	struct elevator_queue *e = q->elevator;

	if (e->type->ops.elevator_set_req_fn)
		return e->type->ops.elevator_set_req_fn(q, rq, bio, gfp_mask);
	return 0;
}

void elv_put_request(struct request_queue *q, struct request *rq)
{
	struct elevator_queue *e = q->elevator;

	if (e->type->ops.elevator_put_req_fn)
		e->type->ops.elevator_put_req_fn(rq);
}

int elv_may_queue(struct request_queue *q, int rw)
{
	struct elevator_queue *e = q->elevator;

	if (e->type->ops.elevator_may_queue_fn)
		return e->type->ops.elevator_may_queue_fn(q, rw);

	return ELV_MQUEUE_MAY;
}

void elv_abort_queue(struct request_queue *q)
{
	struct request *rq;

	blk_abort_flushes(q);

	while (!list_empty(&q->queue_head)) {
		rq = list_entry_rq(q->queue_head.next);
		rq->cmd_flags |= REQ_QUIET;
		trace_block_rq_abort(q, rq);
		/*
		 * Mark this request as started so we don't trigger
		 * any debug logic in the end I/O path.
		 */
		blk_start_request(rq);
		__blk_end_request_all(rq, -EIO);
	}
}
EXPORT_SYMBOL(elv_abort_queue);

void elv_completed_request(struct request_queue *q, struct request *rq)
{
	struct elevator_queue *e = q->elevator;

	/*
	 * request is released from the driver, io must be done
	 */
	if (blk_account_rq(rq)) {
		q->in_flight[rq_is_sync(rq)]--;
		if ((rq->cmd_flags & REQ_SORTED) &&
		    e->type->ops.elevator_completed_req_fn)
			e->type->ops.elevator_completed_req_fn(q, rq);
	}
}

#define to_elv(atr) container_of((atr), struct elv_fs_entry, attr)

static ssize_t
elv_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
	struct elv_fs_entry *entry = to_elv(attr);
	struct elevator_queue *e;
	ssize_t error;

	if (!entry->show)
		return -EIO;

	e = container_of(kobj, struct elevator_queue, kobj);
	mutex_lock(&e->sysfs_lock);
	error = e->type ? entry->show(e, page) : -ENOENT;
	mutex_unlock(&e->sysfs_lock);
	return error;
}

static ssize_t
elv_attr_store(struct kobject *kobj, struct attribute *attr,
	       const char *page, size_t length)
{
	struct elv_fs_entry *entry = to_elv(attr);
	struct elevator_queue *e;
	ssize_t error;

	if (!entry->store)
		return -EIO;

	e = container_of(kobj, struct elevator_queue, kobj);
	mutex_lock(&e->sysfs_lock);
	error = e->type ? entry->store(e, page, length) : -ENOENT;
	mutex_unlock(&e->sysfs_lock);
	return error;
}

static const struct sysfs_ops elv_sysfs_ops = {
	.show	= elv_attr_show,
	.store	= elv_attr_store,
};

static struct kobj_type elv_ktype = {
	.sysfs_ops	= &elv_sysfs_ops,
	.release	= elevator_release,
};

int elv_register_queue(struct request_queue *q)
{
	struct elevator_queue *e = q->elevator;
	int error;

	error = kobject_add(&e->kobj, &q->kobj, "%s", "iosched");
	if (!error) {
		struct elv_fs_entry *attr = e->type->elevator_attrs;
		if (attr) {
			while (attr->attr.name) {
				if (sysfs_create_file(&e->kobj, &attr->attr))
					break;
				attr++;
			}
		}
		kobject_uevent(&e->kobj, KOBJ_ADD);
		e->registered = 1;
	}
	return error;
}
EXPORT_SYMBOL(elv_register_queue);

void elv_unregister_queue(struct request_queue *q)
{
	if (q) {
		struct elevator_queue *e = q->elevator;

		kobject_uevent(&e->kobj, KOBJ_REMOVE);
		kobject_del(&e->kobj);
		e->registered = 0;
	}
}
EXPORT_SYMBOL(elv_unregister_queue);

int elv_register(struct elevator_type *e)
{
	char *def = "";

	/* create icq_cache if requested */
	if (e->icq_size) {
		if (WARN_ON(e->icq_size < sizeof(struct io_cq)) ||
		    WARN_ON(e->icq_align < __alignof__(struct io_cq)))
			return -EINVAL;

		snprintf(e->icq_cache_name, sizeof(e->icq_cache_name),
			 "%s_io_cq", e->elevator_name);
		e->icq_cache = kmem_cache_create(e->icq_cache_name, e->icq_size,
						 e->icq_align, 0, NULL);
		if (!e->icq_cache)
			return -ENOMEM;
	}

	/* register, don't allow duplicate names */
	spin_lock(&elv_list_lock);
	if (elevator_find(e->elevator_name)) {
		spin_unlock(&elv_list_lock);
		if (e->icq_cache)
			kmem_cache_destroy(e->icq_cache);
		return -EBUSY;
	}
	list_add_tail(&e->list, &elv_list);
	spin_unlock(&elv_list_lock);

	/* print pretty message */
	if (!strcmp(e->elevator_name, chosen_elevator) ||
			(!*chosen_elevator &&
			 !strcmp(e->elevator_name, CONFIG_DEFAULT_IOSCHED)))
				def = " (default)";

	printk(KERN_INFO "io scheduler %s registered%s\n", e->elevator_name,
								def);
	return 0;
}
EXPORT_SYMBOL_GPL(elv_register);

void elv_unregister(struct elevator_type *e)
{
	/* unregister */
	spin_lock(&elv_list_lock);
	list_del_init(&e->list);
	spin_unlock(&elv_list_lock);

	/*
	 * Destroy icq_cache if it exists.  icq's are RCU managed.  Make
	 * sure all RCU operations are complete before proceeding.
	 */
	if (e->icq_cache) {
		rcu_barrier();
		kmem_cache_destroy(e->icq_cache);
		e->icq_cache = NULL;
	}
}
EXPORT_SYMBOL_GPL(elv_unregister);

/*
 * switch to new_e io scheduler. be careful not to introduce deadlocks -
 * we don't free the old io scheduler, before we have allocated what we
 * need for the new one. this way we have a chance of going back to the old
 * one, if the new one fails init for some reason.
 */
static int elevator_switch(struct request_queue *q, struct elevator_type *new_e)
{
	struct elevator_queue *old = q->elevator;
	bool registered = old->registered;
	int err;

	/*
	 * Turn on BYPASS and drain all requests w/ elevator private data.
	 * Block layer doesn't call into a quiesced elevator - all requests
	 * are directly put on the dispatch list without elevator data
	 * using INSERT_BACK.  All requests have SOFTBARRIER set and no
	 * merge happens either.
	 */
	blk_queue_bypass_start(q);

	/* unregister and clear all auxiliary data of the old elevator */
	if (registered)
		elv_unregister_queue(q);

	spin_lock_irq(q->queue_lock);
	ioc_clear_queue(q);
	spin_unlock_irq(q->queue_lock);

	/* allocate, init and register new elevator */
	err = new_e->ops.elevator_init_fn(q, new_e);
	if (err)
		goto fail_init;

	if (registered) {
		err = elv_register_queue(q);
		if (err)
			goto fail_register;
	}

	/* done, kill the old one and finish */
	elevator_exit(old);
	blk_queue_bypass_end(q);

	blk_add_trace_msg(q, "elv switch: %s", new_e->elevator_name);

	return 0;

fail_register:
	elevator_exit(q->elevator);
fail_init:
	/* switch failed, restore and re-register old elevator */
	q->elevator = old;
	elv_register_queue(q);
	blk_queue_bypass_end(q);

	return err;
}

/*
 * Switch this queue to the given IO scheduler.
 */
static int __elevator_change(struct request_queue *q, const char *name)
{
	char elevator_name[ELV_NAME_MAX];
	struct elevator_type *e;

	if (!q->elevator)
		return -ENXIO;

	strlcpy(elevator_name, name, sizeof(elevator_name));
	e = elevator_get(strstrip(elevator_name), true);
	if (!e) {
		printk(KERN_ERR "elevator: type %s not found\n", elevator_name);
		return -EINVAL;
	}

	if (!strcmp(elevator_name, q->elevator->type->elevator_name)) {
		elevator_put(e);
		return 0;
	}

	return elevator_switch(q, e);
}

int elevator_change(struct request_queue *q, const char *name)
{
	int ret;

	/* Protect q->elevator from elevator_init() */
	mutex_lock(&q->sysfs_lock);
	ret = __elevator_change(q, name);
	mutex_unlock(&q->sysfs_lock);

	return ret;
}
EXPORT_SYMBOL(elevator_change);

ssize_t elv_iosched_store(struct request_queue *q, const char *name,
			  size_t count)
{
	int ret;

	if (!q->elevator)
		return count;

	ret = __elevator_change(q, name);
	if (!ret)
		return count;

	printk(KERN_ERR "elevator: switch to %s failed\n", name);
	return ret;
}

ssize_t elv_iosched_show(struct request_queue *q, char *name)
{
	struct elevator_queue *e = q->elevator;
	struct elevator_type *elv;
	struct elevator_type *__e;
	int len = 0;

	if (!q->elevator || !blk_queue_stackable(q))
		return sprintf(name, "none\n");

	elv = e->type;

	spin_lock(&elv_list_lock);
	list_for_each_entry(__e, &elv_list, list) {
		if (!strcmp(elv->elevator_name, __e->elevator_name))
			len += sprintf(name+len, "[%s] ", elv->elevator_name);
		else
			len += sprintf(name+len, "%s ", __e->elevator_name);
	}
	spin_unlock(&elv_list_lock);

	len += sprintf(len+name, "\n");
	return len;
}

struct request *elv_rb_former_request(struct request_queue *q,
				      struct request *rq)
{
	struct rb_node *rbprev = rb_prev(&rq->rb_node);

	if (rbprev)
		return rb_entry_rq(rbprev);

	return NULL;
}
EXPORT_SYMBOL(elv_rb_former_request);
//Ӧ�����ҵ�IO�����㷨�������rq����һ��rq���������ò��ֻ�ǿ���rq->rb_node���ɵ�һ�����ζ���ѽ
struct request *elv_rb_latter_request(struct request_queue *q,
				      struct request *rq)
{
	struct rb_node *rbnext = rb_next(&rq->rb_node);

	if (rbnext)
		return rb_entry_rq(rbnext);

	return NULL;
}
EXPORT_SYMBOL(elv_rb_latter_request);
