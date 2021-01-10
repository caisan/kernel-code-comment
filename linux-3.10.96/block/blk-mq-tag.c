/*
 * Tag allocation using scalable bitmaps. Uses active queue tracking to support
 * fairer distribution of tags between multiple submitters when a shared tag map
 * is used.
 *
 * Copyright (C) 2013-2014 Jens Axboe
 */
#include <linux/kernel.h>
#include <linux/module.h>

#include <linux/blk-mq.h>
#include "blk.h"
#include "blk-mq.h"
#include "blk-mq-tag.h"

bool blk_mq_has_free_tags(struct blk_mq_tags *tags)
{
	if (!tags)
		return true;

	return sbitmap_any_bit_clear(&tags->bitmap_tags.sb);
}

/*
 * If a previously inactive queue goes active, bump the active user count.
 */
bool __blk_mq_tag_busy(struct blk_mq_hw_ctx *hctx)
{
	if (!test_bit(BLK_MQ_S_TAG_ACTIVE, &hctx->state) &&
	    !test_and_set_bit(BLK_MQ_S_TAG_ACTIVE, &hctx->state))
		atomic_inc(&hctx->tags->active_queues);

	return true;
}

/*
 * Wakeup all potentially sleeping on tags
 */
void blk_mq_tag_wakeup_all(struct blk_mq_tags *tags, bool include_reserve)
{
	sbitmap_queue_wake_all(&tags->bitmap_tags);
	if (include_reserve)
		sbitmap_queue_wake_all(&tags->breserved_tags);
}

/*
 * If a previously busy queue goes inactive, potential waiters could now
 * be allowed to queue. Wake them up and check.
 */
void __blk_mq_tag_idle(struct blk_mq_hw_ctx *hctx)
{
	struct blk_mq_tags *tags = hctx->tags;

	if (!test_and_clear_bit(BLK_MQ_S_TAG_ACTIVE, &hctx->state))
		return;

	atomic_dec(&tags->active_queues);

	blk_mq_tag_wakeup_all(tags, false);
}

/*
 * For shared tag users, we track the number of currently active users
 * and attempt to provide a fair share of the tag depth for each of them.
 */
static inline bool hctx_may_queue(struct blk_mq_hw_ctx *hctx,
				  struct sbitmap_queue *bt)
{
	unsigned int depth, users;

	if (!hctx || !(hctx->flags & BLK_MQ_F_TAG_SHARED))
		return true;
	if (!test_bit(BLK_MQ_S_TAG_ACTIVE, &hctx->state))
		return true;

	/*
	 * Don't try dividing an ant
	 */
	if (bt->sb.depth == 1)
		return true;

	users = atomic_read(&hctx->tags->active_queues);
	if (!users)
		return true;

	/*
	 * Allow at least some tags
	 */
	depth = max((bt->sb.depth + users - 1) / users, 4U);
	return atomic_read(&hctx->nr_active) < depth;
}
//����sbitmap_queue�õ�blk_mq_tags�ṹ���static_rqs[]��������е�request�������±꣬��������±꣬ʵ������±겻��static_rqs[]
//�����������±꣬��һ��ƫ��ֵ����
static int __blk_mq_get_tag(struct blk_mq_alloc_data *data,
			    struct sbitmap_queue *bt)
{
	if (!(data->flags & BLK_MQ_REQ_INTERNAL) &&
	    !hctx_may_queue(data->hctx, bt))
		return -1;
	if (data->shallow_depth)
		return __sbitmap_queue_get_shallow(bt, data->shallow_depth);
	else
		return __sbitmap_queue_get(bt);
}
/*
1 ����Ӳ�����е�tags->breserved_tags��tags->bitmap_tags��static_rqs[]��nr_reserved_tagsһֱ���ɻ�����Ӧ�ø�����ˡ���submioʱִ��
blk_mq_make_request->blk_mq_sched_get_request����Ӳ��������ص�blk_mq_tags�ṹ��static_rqs[]������õ����е�req����ʵ������:
�ȵõ�Ӳ������hctx��Ȼ��������޵����㷨���ظ�Ӳ��Ψһ�󶨵�hctx->sched_tags����hctx->tags����blk_mq_get_tag()�е�
struct blk_mq_tags *tags = blk_mq_tags_from_data(data)����������blk_mq_tags�����Ŵ�tags->breserved_tags����tags->bitmap_tags�ȷ���
һ������tag�����tagָ���˱��η����req��static_rqs[]������±꣬�±����blk_mq_get_tag()�ķ���ֵtag + tag_offset��
tags->breserved_tags����tags->bitmap_tags��struct sbitmap_queue�ṹ��Ӧ�ÿ������ɾ���һ����bitλ�ɣ�
�е���ext4�ļ�ϵͳ��inode bitmap��ÿһ��bit��ʾһ��tag����bit��ʾ��tag�������˾���1������tagӦ�þ��Ǵ�tags->breserved_tags����
tags->bitmap_tags����bitΪ��0���ĸ�?Ӧ���������˼��Ȼ��ֵreq->tag =tag ��hctx->tags->rqs[req->tag] = req��

2 ��tags->bitmap_tags����tags->breserved_tags�����tag����ʵ��һ�����֣���ʾ���η����reg��static_rqs[]������±ꡣ

3 ����tags->breserved_tags��tags->bitmap_tags��
��blk_mq_get_tag()����if (data->flags & BLK_MQ_REQ_RESERVED)��������ʹ��tags->breserved_tags��ʲô����������?

submioִ��blk_mq_make_request->blk_mq_sched_get_request��ʹ���˵���������data->flags |= BLK_MQ_REQ_INTERNAL��

Ȼ��ִ��blk_mq_get_tag(),if (data->flags & BLK_MQ_REQ_RESERVED��������ִ��bt = &tags->bitmap_tags��tag_offset = tags->nr_reserved_tags��
Ȼ���tags->bitmap_tags����һ��tag��Ȼ��tags->nr_reserved_tags+tag �Ǳ��η����req��static_rqs[]���±꣬ɶ��˼?static_rqs[]�����
0~tags->nr_reserved_tagsλ�ö���reserved tag��tags->nr_reserved_tags��ߵĲ��Ƿ�reserved tag������ִ��__blk_mq_alloc_request(),
��Ϊif (data->flags & BLK_MQ_REQ_INTERNAL)��������__rq_aux(rq, data->q)->internal_tag = tag�����tag����tags->nr_reserved_tags��
������Ҫ���Ժ�����á�Ȼ�󾭹���������;��Ҫ�Ѹ�req���͸�Ӳ�������ˣ���ִ��blk_mq_dispatch_rq_list()��������Ϊ��������Ӳ����æ��
��reqû���ɷ��ɹ�����Ҫִ��__blk_mq_requeue_request(req)���Ѹ�reqռ�õ�tag��tags->bitmap_tags���ͷŵ���Ȼ���req����hctx->dispatch
���������첽�ɷ������ջ���ִ��blk_mq_dispatch_rq_list()->blk_mq_get_driver_tag()��
if (blk_mq_tag_is_reserved(data.hctx->sched_tags, rq_aux(rq)->internal_tag))������������ִ��data.flags |= BLK_MQ_REQ_RESERVED��
����ִ��blk_mq_get_tag()�����ϱߵ�����һ��������bt = &tags->bitmap_tags ��bitmap_tags����tag��

4���submioִ��blk_mq_make_request->blk_mq_sched_get_request��û��ʹ�õ�����������ִ��data->flags |= BLK_MQ_REQ_INTERNAL��

Ȼ��ִ��blk_mq_get_tag(),if (data->flags & BLK_MQ_REQ_RESERVED��������ִ��bt = &tags->bitmap_tags��tag_offset = tags->nr_reserved_tags��
Ȼ���tags->bitmap_tags����һ��tag��Ȼ��tags->nr_reserved_tags+tag �Ǳ��η����req��static_rqs[]���±ꡣ����ִ��
__blk_mq_alloc_request(),��Ϊif (data->flags & BLK_MQ_REQ_INTERNAL)����������������__rq_aux(rq, data->q)->internal_tag = -1��
����tagС��tags->nr_reserved_tags��������Ҫ���Ժ�����á�Ȼ�󾭹���������;��Ҫ�Ѹ�req���͸�Ӳ�������ˣ���ִ��
blk_mq_dispatch_rq_list()��������Ϊ��������Ӳ����æ����reqû���ɷ��ɹ�����Ҫִ��__blk_mq_requeue_request(req)���Ѹ�reqռ�õ�
tag��tags->bitmap_tags���ͷŵ���Ȼ���req����hctx->dispatch���������첽�ɷ������ջ���ִ��blk_mq_dispatch_rq_list()->
blk_mq_get_driver_tag()��if (blk_mq_tag_is_reserved(data.hctx->sched_tags, rq_aux(rq)->internal_tag))����������������������
��ִ��data.flags |= BLK_MQ_REQ_RESERVED������ִ��blk_mq_get_tag()����Ϊif (data->flags & BLK_MQ_REQ_RESERVED) ��������
bt = &tags->breserved_tags��tag_offset = 0���򱾴��Ǵ�tags->breserved_tags���reserved tag����tag������tag+0�Ǳ��η����req��
static_rqs[]������±ꡣҲ����˵��static_rqs[]�����0~tags->nr_reserved_tags��reserved tag��req�������±꣬tags->nr_reserved_tags����
��tag�Ƿ�reserved tag��req�������±ꡣ

!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!�����ˣ�����������req�ӳ��η��䣬reqʼ��û�б䣬����tag�����ͷ��ٷ��䣬tag����ˣ�tag����Ψһ
��ʾreq��static_rqs[]�����λ���𣬲��а�req�ǹ̶��ģ�����tag������ȥ

5 ����ִ��blk_mq_get_driver_tag()����������Ǹ�req�ڵ�һ���ɷ�ʱ����Ӳ�����з�æ���Ͱ�tag�ͷ��ˣ�Ȼ��rq->tag=-1��
���������첽�ɷ����Ż�ִ�иú���

6 tag��req�ǰ󶨵ģ���submioִ��blk_mq_make_request->blk_mq_sched_get_requestʱ�����ȴ�Ӳ������blk_mq_tags����tag��Ȼ���blk_mq_tags
��static_rqs[tag]�õ�req��֮���ڽ���req����ʱ��������������Ӳ����æ�����ִ��__blk_mq_requeue_request(req)��req��tag�ͷŵ���Ȼ���
�첽�ɷ�reqʱ�����ִ��blk_mq_get_driver_tag()����Ϊreq����tag��
*/

//��Ӳ�����е�blk_mq_tags�ṹ���tags->bitmap_tags����tags->nr_reserved_tags����һ������tag��һ��req�������һ��tag����IO���䡣
//����ʧ��������Ӳ��IO�����ɷ������ߣ����ٳ��Դ�blk_mq_tags�ṹ���tags->bitmap_tags����tags->nr_reserved_tags����һ������tag��
unsigned int blk_mq_get_tag(struct blk_mq_alloc_data *data)
{
    //ʹ�õ�����ʱ����Ӳ�����е�hctx->sched_tags���޵�����ʱ����Ӳ�����е�hctx->tags�����ص���Ӳ������Ψһ��Ӧ�ĵ�blk_mq_tags
	struct blk_mq_tags *tags = blk_mq_tags_from_data(data);
	struct sbitmap_queue *bt;
	struct sbq_wait_state *ws;
	DEFINE_WAIT(wait);
	unsigned int tag_offset;
	bool drop_ctx;
	int tag;

	if (data->flags & BLK_MQ_REQ_RESERVED) {//ʹ��Ԥ��tag
		if (unlikely(!tags->nr_reserved_tags)) {
			WARN_ON_ONCE(1);
			return BLK_MQ_TAG_FAIL;
		}
		bt = &tags->breserved_tags;
		tag_offset = 0;
	} else {//��ʹ��Ԥ��tag
	
	    //����blk_mq_tags��bitmap_tags
		bt = &tags->bitmap_tags;
        //Ӧ����static_rqs[]����е�request�������±�ƫ�ƣ����ú������
		tag_offset = tags->nr_reserved_tags;
	}
    
//��Ӳ�����е�blk_mq_tags�ṹ���tags->bitmap_tags����tags->nr_reserved_tags����һ������tag��tag������req��static_rqs[]�������±ꡣ
//ʵ��tag������req��static_rqs[]������±꣬�����������±꣬��һ��ƫ��ֵtag_offset���ǡ�����-1˵��û�п���tag���ͻ�ִ���±ߵ�ѭ����
//��������Ӳ�����䣬���ڳ����е�tag��һ��tag����һ��req��req����ǰ����÷���tag������tag�����Ǵ�Ӳ������blk_mq_tags�õ�����req��
	tag = __blk_mq_get_tag(data, bt);
	if (tag != -1)
		goto found_tag;

	if (data->flags & BLK_MQ_REQ_NOWAIT)//��Ȼ���ǲ����еȴ�
		return BLK_MQ_TAG_FAIL;

    //�ߵ���һ����˵��Ӳ�������йص�blk_mq_tags��û�п��е�request�ɷ��䣬�Ǿͻ��������ߵȴ�������ִ��blk_mq_run_hw_queue
    //����IO ���ݴ��䣬������ɺ�����ͷų�request���ﵽ����request��Ŀ��

	ws = bt_wait_ptr(bt, data->hctx);//��ȡӲ������Ψһ��Ӧ��wait_queue_head_t�ȴ�����ͷ����ȥ����Ҳ��Ӳ������Ψһ��Ӧ��
	drop_ctx = data->ctx == NULL;
	do {
		struct sbitmap_queue *bt_prev;

		prepare_to_wait(&ws->wait, &wait, TASK_UNINTERRUPTIBLE);//��ws->wait�ȴ�����׼������
        
        //�ٴγ��Դ�blk_mq_tags�ṹ����������tag
		tag = __blk_mq_get_tag(data, bt);
		if (tag != -1)
			break;

		/*
		 * We're out of tags on this hardware queue, kick any
		 * pending IO submits before going to sleep waiting for
		 * some to complete.
		 */
		//��������Ӳ������IOͬ�����䣬���ڳ�����req
		blk_mq_run_hw_queue(data->hctx, false);

		/*
		 * Retry tag allocation after running the hardware queue,
		 * as running the queue may also have found completions.
		 */
		//�ٴγ��Դ�blk_mq_tags�ṹ����������tag
		tag = __blk_mq_get_tag(data, bt);
		if (tag != -1)
			break;

		if (data->ctx)
			blk_mq_put_ctx(data->ctx);

		bt_prev = bt;
        //���ߵ���
		io_schedule();

        //��֣��ٴλ�ȡ������к�Ӳ�����У�Ϊʲô?????????�ϱ�������Ӳ��IO�����ɷ�����io_schedule()���Ⱥ��ٱ����ѣ���������CPU��
        //���ܻ�䣬����Ҫ���ݽ�������CPU��ȡ��Ӧ��������У��ٻ�ȡ��Ӧ��Ӳ������
		data->ctx = blk_mq_get_ctx(data->q);
		data->hctx = blk_mq_map_queue(data->q, data->ctx->cpu);
        
        //ʹ�õ�����ʱ����Ӳ�����е�hctx->sched_tags���޵�����ʱ����Ӳ�����е�hctx->tags
		tags = blk_mq_tags_from_data(data);
		if (data->flags & BLK_MQ_REQ_RESERVED)
			bt = &tags->breserved_tags;
		else//�ٴλ�ȡbitmap_tags�����̸�ǰ��һģһ��
			bt = &tags->bitmap_tags;

        //���ߺ��ѣ��������
		finish_wait(&ws->wait, &wait);

		/*
		 * If destination hw queue is changed, fake wake up on
		 * previous queue for compensating the wake up miss, so
		 * other allocations on previous queue won't be starved.
		 */
		if (bt != bt_prev)
			sbitmap_queue_wake_up(bt_prev);

        //�ٴθ���Ӳ�����л�ȡΨһ��Ӧ��wait_queue_head_t�ȴ�����ͷ
		ws = bt_wait_ptr(bt, data->hctx);
	} while (1);

	if (drop_ctx && data->ctx)
		blk_mq_put_ctx(data->ctx);

	finish_wait(&ws->wait, &wait);

found_tag:
    //����û�У�tag+tag_offset���Ǳ��η���Ŀ���request��static_rqs[]����������±�
	return tag + tag_offset;
}
//tags->bitmap_tags�а���req->tag���tag����ͷ�tag
void blk_mq_put_tag(struct blk_mq_hw_ctx *hctx, struct blk_mq_tags *tags,
		    struct blk_mq_ctx *ctx, unsigned int tag)
{
	if (!blk_mq_tag_is_reserved(tags, tag)) {
        //tag - tags->nr_reserved_tags����Ǹ�tag��tags->bitmap_tags������λ��
		const int real_tag = tag - tags->nr_reserved_tags;

		BUG_ON(real_tag >= tags->nr_tags);
		sbitmap_queue_clear(&tags->bitmap_tags, real_tag, ctx->cpu);
	} else {
		BUG_ON(tag >= tags->nr_reserved_tags);
		sbitmap_queue_clear(&tags->breserved_tags, tag, ctx->cpu);
	}
}

struct bt_iter_data {
	struct blk_mq_hw_ctx *hctx;
	busy_iter_fn *fn;
	void *data;
	bool reserved;
};

static bool bt_iter(struct sbitmap *bitmap, unsigned int bitnr, void *data)
{
	struct bt_iter_data *iter_data = data;
	struct blk_mq_hw_ctx *hctx = iter_data->hctx;
	struct blk_mq_tags *tags = hctx->tags;
	bool reserved = iter_data->reserved;
	struct request *rq;

	if (!reserved)
		bitnr += tags->nr_reserved_tags;
	rq = tags->rqs[bitnr];

	/*
	 * We can hit rq == NULL here, because the tagging functions
	 * test and set the bit before assining ->rqs[].
	 */
	if (rq && rq->q == hctx->queue)
		iter_data->fn(hctx, rq, iter_data->data, reserved);
	return true;
}

static void bt_for_each(struct blk_mq_hw_ctx *hctx, struct sbitmap_queue *bt,
			busy_iter_fn *fn, void *data, bool reserved)
{
	struct bt_iter_data iter_data = {
		.hctx = hctx,
		.fn = fn,
		.data = data,
		.reserved = reserved,
	};

	sbitmap_for_each_set(&bt->sb, bt_iter, &iter_data);
}

struct bt_tags_iter_data {
	struct blk_mq_tags *tags;
	busy_tag_iter_fn *fn;
	void *data;
	bool reserved;
};

static bool bt_tags_iter(struct sbitmap *bitmap, unsigned int bitnr, void *data)
{
	struct bt_tags_iter_data *iter_data = data;
	struct blk_mq_tags *tags = iter_data->tags;
	bool reserved = iter_data->reserved;
	struct request *rq;

	if (!reserved)
		bitnr += tags->nr_reserved_tags;

	/*
	 * We can hit rq == NULL here, because the tagging functions
	 * test and set the bit before assining ->rqs[].
	 */
	rq = tags->rqs[bitnr];
	if (rq)
		iter_data->fn(rq, iter_data->data, reserved);

	return true;
}

static void bt_tags_for_each(struct blk_mq_tags *tags, struct sbitmap_queue *bt,
			     busy_tag_iter_fn *fn, void *data, bool reserved)
{
	struct bt_tags_iter_data iter_data = {
		.tags = tags,
		.fn = fn,
		.data = data,
		.reserved = reserved,
	};

	if (tags->rqs)
		sbitmap_for_each_set(&bt->sb, bt_tags_iter, &iter_data);
}

static void blk_mq_all_tag_busy_iter(struct blk_mq_tags *tags,
		busy_tag_iter_fn *fn, void *priv)
{
	if (tags->nr_reserved_tags)
		bt_tags_for_each(tags, &tags->breserved_tags, fn, priv, true);
	bt_tags_for_each(tags, &tags->bitmap_tags, fn, priv, false);
}

void blk_mq_tagset_busy_iter(struct blk_mq_tag_set *tagset,
		busy_tag_iter_fn *fn, void *priv)
{
	int i;

	for (i = 0; i < tagset->nr_hw_queues; i++) {
		if (tagset->tags && tagset->tags[i])
			blk_mq_all_tag_busy_iter(tagset->tags[i], fn, priv);
	}
}
EXPORT_SYMBOL(blk_mq_tagset_busy_iter);

int blk_mq_reinit_tagset(struct blk_mq_tag_set *set)
{
	int i, j, ret = 0;

	if (!set->ops->aux_ops || !set->ops->aux_ops->reinit_request)
		goto out;

	for (i = 0; i < set->nr_hw_queues; i++) {
		struct blk_mq_tags *tags = set->tags[i];

		if (!tags)
			continue;

		for (j = 0; j < tags->nr_tags; j++) {
			if (!tags->static_rqs[j])
				continue;

			ret = set->ops->aux_ops->reinit_request(set->driver_data,
						tags->static_rqs[j]);
			if (ret)
				goto out;
		}
	}

out:
	return ret;
}
EXPORT_SYMBOL_GPL(blk_mq_reinit_tagset);

void blk_mq_queue_tag_busy_iter(struct request_queue *q, busy_iter_fn *fn,
		void *priv)
{
	struct blk_mq_hw_ctx *hctx;
	int i;


	queue_for_each_hw_ctx(q, hctx, i) {
		struct blk_mq_tags *tags = hctx->tags;

		/*
		 * If not software queues are currently mapped to this
		 * hardware queue, there's nothing to check
		 */
		if (!blk_mq_hw_queue_mapped(hctx))
			continue;

		if (tags->nr_reserved_tags)
			bt_for_each(hctx, &tags->breserved_tags, fn, priv, true);
		bt_for_each(hctx, &tags->bitmap_tags, fn, priv, false);
	}

}

static int bt_alloc(struct sbitmap_queue *bt, unsigned int depth,
		    bool round_robin, int node)
{
	return sbitmap_queue_init_node(bt, depth, -1, round_robin, GFP_KERNEL,
				       node);
}

static struct blk_mq_tags *blk_mq_init_bitmap_tags(struct blk_mq_tags *tags,
						   int node, int alloc_policy)
{
	unsigned int depth = tags->nr_tags - tags->nr_reserved_tags;
	bool round_robin = alloc_policy == BLK_TAG_ALLOC_RR;

	if (bt_alloc(&tags->bitmap_tags, depth, round_robin, node))
		goto free_tags;
	if (bt_alloc(&tags->breserved_tags, tags->nr_reserved_tags, round_robin,
		     node))
		goto free_bitmap_tags;

	return tags;
free_bitmap_tags:
	sbitmap_queue_free(&tags->bitmap_tags);
free_tags:
	kfree(tags);
	return NULL;
}
//����һ��blk_mq_tags�ṹ���������Աnr_reserved_tags��nr_tags������blk_mq_tags��bitmap_tags��breserved_tags�ṹ
struct blk_mq_tags *blk_mq_init_tags(unsigned int total_tags,
				     unsigned int reserved_tags,
				     int node, int alloc_policy)
{
	struct blk_mq_tags *tags;
    //total_tags��Ȼ��set->queue_depth
	if (total_tags > BLK_MQ_TAG_MAX) {
		pr_err("blk-mq: tag depth too large\n");
		return NULL;
	}
    //����һ��blk_mq_tags�ṹ���������Աnr_reserved_tags��nr_tags
	tags = kzalloc_node(sizeof(*tags), GFP_KERNEL, node);
	if (!tags)
		return NULL;

	tags->nr_tags = total_tags;
	tags->nr_reserved_tags = reserved_tags;
    //����blk_mq_tags��bitmap_tags��breserved_tags�ṹ
	return blk_mq_init_bitmap_tags(tags, node, alloc_policy);
}

void blk_mq_free_tags(struct blk_mq_tags *tags)
{
	sbitmap_queue_free(&tags->bitmap_tags);
	sbitmap_queue_free(&tags->breserved_tags);
	kfree(tags);
}

int blk_mq_tag_update_depth(struct blk_mq_hw_ctx *hctx,
			    struct blk_mq_tags **tagsptr, unsigned int tdepth,
			    bool can_grow)
{
	struct blk_mq_tags *tags = *tagsptr;

	if (tdepth <= tags->nr_reserved_tags)
		return -EINVAL;

	tdepth -= tags->nr_reserved_tags;

	/*
	 * If we are allowed to grow beyond the original size, allocate
	 * a new set of tags before freeing the old one.
	 */
	if (tdepth > tags->nr_tags) {
		struct blk_mq_tag_set *set = hctx->queue->tag_set;
		struct blk_mq_tags *new;
		bool ret;

		if (!can_grow)
			return -EINVAL;

		/*
		 * We need some sort of upper limit, set it high enough that
		 * no valid use cases should require more.
		 */
		if (tdepth > 16 * BLKDEV_MAX_RQ)
			return -EINVAL;

		new = blk_mq_alloc_rq_map(set, hctx->queue_num, tdepth, 0);
		if (!new)
			return -ENOMEM;
		ret = blk_mq_alloc_rqs(set, new, hctx->queue_num, tdepth);
		if (ret) {
			blk_mq_free_rq_map(new);
			return -ENOMEM;
		}

		blk_mq_free_rqs(set, *tagsptr, hctx->queue_num);
		blk_mq_free_rq_map(*tagsptr);
		*tagsptr = new;
	} else {
		/*
		 * Don't need (or can't) update reserved tags here, they
		 * remain static and should never need resizing.
		 */
		sbitmap_queue_resize(&tags->bitmap_tags, tdepth);
	}

	return 0;
}

/**
 * blk_mq_unique_tag() - return a tag that is unique queue-wide
 * @rq: request for which to compute a unique tag
 *
 * The tag field in struct request is unique per hardware queue but not over
 * all hardware queues. Hence this function that returns a tag with the
 * hardware context index in the upper bits and the per hardware queue tag in
 * the lower bits.
 *
 * Note: When called for a request that is queued on a non-multiqueue request
 * queue, the hardware context index is set to zero.
 */
u32 blk_mq_unique_tag(struct request *rq)
{
	struct request_queue *q = rq->q;
	struct blk_mq_hw_ctx *hctx;
	int hwq = 0;

	if (q->mq_ops) {
		hctx = blk_mq_map_queue(q, rq->mq_ctx->cpu);
		hwq = hctx->queue_num;
	}

	return (hwq << BLK_MQ_UNIQUE_TAG_BITS) |
		(rq->tag & BLK_MQ_UNIQUE_TAG_MASK);
}
EXPORT_SYMBOL(blk_mq_unique_tag);
