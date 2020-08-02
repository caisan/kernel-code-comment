/*
 * blk-mq scheduling framework
 *
 * Copyright (C) 2016 Jens Axboe
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/blk-mq.h>

#include <trace/events/block.h>

#include "blk.h"
#include "blk-mq.h"
#include "blk-mq-debugfs.h"
#include "blk-mq-sched.h"
#include "blk-mq-tag.h"

void blk_mq_sched_free_hctx_data(struct request_queue *q,
				 void (*exit)(struct blk_mq_hw_ctx *))
{
	struct blk_mq_hw_ctx *hctx;
	int i;

	queue_for_each_hw_ctx(q, hctx, i) {
		if (exit && hctx->sched_data)
			exit(hctx);
		kfree(hctx->sched_data);
		hctx->sched_data = NULL;
	}
}
EXPORT_SYMBOL_GPL(blk_mq_sched_free_hctx_data);

static void __blk_mq_sched_assign_ioc(struct request_queue *q,
				      struct request *rq,
				      struct bio *bio,
				      struct io_context *ioc)
{
	struct io_cq *icq;

	spin_lock_irq(q->queue_lock);
	icq = ioc_lookup_icq(ioc, q);
	spin_unlock_irq(q->queue_lock);

	if (!icq) {
		icq = ioc_create_icq(ioc, q, GFP_ATOMIC);
		if (!icq)
			return;
	}

	rq->elv.icq = icq;
	if (!blk_mq_sched_get_rq_priv(q, rq, bio)) {
		rq->cmd_flags |= REQ_ELVPRIV;
		get_io_context(icq->ioc);
		return;
	}

	rq->elv.icq = NULL;
}

static void blk_mq_sched_assign_ioc(struct request_queue *q,
				    struct request *rq, struct bio *bio)
{
	struct io_context *ioc;

	ioc = rq_ioc(bio);
	if (ioc)
		__blk_mq_sched_assign_ioc(q, rq, bio, ioc);
}

/*
 * Mark a hardware queue as needing a restart. For shared queues, maintain
 * a count of how many hardware queues are marked for restart.
 */
//���hctx->state��BLK_MQ_S_SCHED_RESTART��־λ
static void blk_mq_sched_mark_restart_hctx(struct blk_mq_hw_ctx *hctx)
{
	if (test_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state))
		return;

	if (hctx->flags & BLK_MQ_F_TAG_SHARED) {
		struct request_queue *q = hctx->queue;

		if (!test_and_set_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state))
			atomic_inc(&q->shared_hctx_restart);
	} else
		set_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state);
}

static bool blk_mq_sched_restart_hctx(struct blk_mq_hw_ctx *hctx)
{
	if (!test_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state))
		return false;

	if (hctx->flags & BLK_MQ_F_TAG_SHARED) {
		struct request_queue *q = hctx->queue;

		if (test_and_clear_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state))
			atomic_dec(&q->shared_hctx_restart);
	} else
		clear_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state);

	return blk_mq_run_hw_queue(hctx, true);
}
//��Ӳ�������йص�blk_mq_tags�ṹ���static_rqs[]������õ����е�request����ȡʧ��������Ӳ��IO�����ɷ���
//֮���ٳ��Դ�blk_mq_tags�ṹ���static_rqs[]������õ����е�request�����ء�
struct request *blk_mq_sched_get_request(struct request_queue *q,
					 struct bio *bio,
					 unsigned int op,
					 struct blk_mq_alloc_data *data)
{
	struct elevator_queue *e = q->elevator;
	struct request *rq;
	const bool is_flush = op & (REQ_FLUSH | REQ_FUA);

	blk_queue_enter_live(q);
	data->q = q;
    
	if (likely(!data->ctx))
		data->ctx = blk_mq_get_ctx(q);
	if (likely(!data->hctx))
		data->hctx = blk_mq_map_queue(q, data->ctx->cpu);

	if (e) {//�е�����
		data->flags |= BLK_MQ_REQ_INTERNAL;//�е���ʱ������BLK_MQ_REQ_INTERNAL��־

		/*
		 * Flush requests are special and go directly to the
		 * dispatch list.
		 */
		if (!is_flush && e->aux->ops.mq.get_request) {
			rq = e->aux->ops.mq.get_request(q, op, data);
			if (rq)
				rq->cmd_flags |= REQ_QUEUED;
		} else
		    //��Ӳ�������йص�blk_mq_tags�ṹ���static_rqs[]������õ����е�request����ȡʧ��������Ӳ��IO�����ɷ���
            //֮���ٳ��Դ�blk_mq_tags�ṹ���static_rqs[]������õ����е�request�����ء�
			rq = __blk_mq_alloc_request(data, op);
	} else {//�޵�����
	
        //ͬ��
		rq = __blk_mq_alloc_request(data, op);
	}

	if (rq) {
		if (!is_flush) {
			rq->elv.icq = NULL;
			if (e && e->type->icq_cache)
				blk_mq_sched_assign_ioc(q, rq, bio);
		}
		data->hctx->queued++;
		return rq;
	}

	blk_queue_exit(q);
	return NULL;
}

void blk_mq_sched_put_request(struct request *rq)
{
	struct request_queue *q = rq->q;
	struct elevator_queue *e = q->elevator;

	if (rq->cmd_flags & REQ_ELVPRIV) {
		blk_mq_sched_put_rq_priv(rq->q, rq);
		if (rq->elv.icq) {
			put_io_context(rq->elv.icq->ioc);
			rq->elv.icq = NULL;
		}
	}

	if ((rq->cmd_flags & REQ_QUEUED) && e && e->aux->ops.mq.put_request)
		e->aux->ops.mq.put_request(rq);
	else
		blk_mq_finish_request(rq);
}

/*
 * Only SCSI implements .get_budget and .put_budget, and SCSI restarts
 * its queue by itself in its completion handler, so we don't need to
 * restart queue if .get_budget() returns BLK_STS_NO_RESOURCE.
 */
//ִ��deadline�㷨�ɷ�������ѭ����fifo���ߺ��������ѡ����ɷ��������req��Ȼ���req��Ӳ������hctx��blk_mq_tags�����һ������tag��
//���ǽ���req��Ӳ�����е���ϵ�ɡ�Ȼ��ֱ������nvmeӲ�����䡣���nvmeӲ�����з�æ�����reqת�Ƶ�hctx->dispatch���У�Ȼ������nvme�첽
//���䡣Ӳ�����з�æ����deadline�㷨����û��req��������ѭ����
static void blk_mq_do_dispatch_sched(struct blk_mq_hw_ctx *hctx)
{
	struct request_queue *q = hctx->queue;
	struct elevator_queue *e = q->elevator;
	LIST_HEAD(rq_list);

	do {
		struct request *rq;

		if (e->aux->ops.mq.has_work &&
				!e->aux->ops.mq.has_work(hctx))//dd_has_work���ж���reqҪ����ɣ�����break
			break;

		if (!blk_mq_get_dispatch_budget(hctx))
			break;

 //ִ��deadline�㷨�ɷ���������fifo���ߺ��������ѡ����ɷ���req���ء�Ȼ�������µ�next_rq������req��fifo���кͺ���������޳���
 //req��Դ��:�ϴ��ɷ����õ�next_rq;read req�ɷ������ѡ���write req;fifo �����ϳ�ʱҪ�����req��ͳ���ˣ��й̶�����
		rq = e->aux->ops.mq.dispatch_request(hctx);//dd_dispatch_request
		if (!rq) {
			blk_mq_put_dispatch_budget(hctx);
			break;
		}

		/*
		 * Now this rq owns the budget which has to be released
		 * if this rq won't be queued to driver via .queue_rq()
		 * in blk_mq_dispatch_rq_list().
		 */
		//��ѡ������ɷ���req����ֲ�����rq_list����
		list_add(&rq->queuelist, &rq_list);

//blk_mq_dispatch_rq_list����:����rq_list�ϵ�req���ȸ�req��Ӳ������hctx��blk_mq_tags�����һ������tag������
//����req��Ӳ�����е���ϵ�ɣ�Ȼ��ֱ������nvmeӲ�����䡣������һ��reqҪ����Ӳ�����䣬��Ҫ��blk_mq_tags�ṹ��õ�һ�����е�tag��
//���nvmeӲ�����з�æ����Ҫ��rq_listʣ���reqת�Ƶ�hctx->dispatch���У�Ȼ������nvme�첽���䡣Ӳ�����з�æ����flase!!!!!!

//�������Ҿ��������⣬rq_list������ɶ��?ÿ��dd_dispatch_request���㷨������ȡ��һ�����ɷ���req���ŵ�rq_list�����ž�ִ��
//blk_mq_dispatch_rq_list����req���䣬rq_listֻ��һ��reqѽ��Ϊʲô������ܼ���req��rq_list��ִ��blk_mq_dispatch_rq_list��??????????
	}while (blk_mq_dispatch_rq_list(q, &rq_list, true));//Ӳ�����з�æ����rq_list������򷵻�flase������ѭ��
    
}

static struct blk_mq_ctx *blk_mq_next_ctx(struct blk_mq_hw_ctx *hctx,
					  struct blk_mq_ctx *ctx)
{
    //Ӳ������hctx�����ĵ�ctx->index_hw�����������ctx
	unsigned idx = ctx->index_hw;

    //��Ȼ�ﵽӲ�����й���������������������ӹ�����0��������п�ʼ
	if (++idx == hctx->nr_ctx)
		idx = 0;
    //����Ӳ�����й����ĵ�idx���������
	return hctx->ctxs[idx];
}

/*
 * Only SCSI implements .get_budget and .put_budget, and SCSI restarts
 * its queue by itself in its completion handler, so we don't need to
 * restart queue if .get_budget() returns BLK_STS_NO_RESOURCE.
 */
//����ѭ������hctxӲ�����й���������������У�����ȡ��һ���������ctx->rq_list�ϵ�req����rq_list�ֲ�����ִ��blk_mq_dispatch_rq_list()Ӳ���ɷ�req��
//���nvmeӲ�����з�æ����Ҫ��rq_listʣ���reqת�Ƶ�hctx->dispatch���У�Ȼ������nvme�첽���䡣ѭ���˳������ǣ�nvmeӲ�����з�æ
//����hctxӲ�����й�����������������ϵ�reqȫ���ɷ��ꡣ�и����ʣ������nvmeӲ�����з�æ�����п�����Щ��������ϵ�req��û���ü��ɷ�ѽ?????????????
static void blk_mq_do_dispatch_ctx(struct blk_mq_hw_ctx *hctx)
{
	struct request_queue *q = hctx->queue;
	LIST_HEAD(rq_list);
	struct blk_mq_ctx *ctx = READ_ONCE(hctx->dispatch_from);

    //���α���hctxӲ�����й����������������
	do {
		struct request *rq;

        //��Ӧ���Ǽ��Ӳ�����й��������������û�д������req��???????
		if (!sbitmap_any_bit_set(&hctx->ctx_map))
			break;

		if (!blk_mq_get_dispatch_budget(hctx))
			break;
        
        //�����ctx->rq_listȡ��req��Ȼ�������������޳�req���������hctx->ctx_map��������ж�Ӧ�ı�־λ???????
		rq = blk_mq_dequeue_from_ctx(hctx, ctx);
		if (!rq) {
			blk_mq_put_dispatch_budget(hctx);
			break;
		}
    /*�����������ϵ�req���ɷ����ҿ��Ÿ��ԣ�һ��ֻ���������ȡ��һ��req��Ȼ�����blk_mq_dispatch_rq_listӲ���ɷ���
      Ȼ���ȡ��Ӳ�����й�������һ��������У���ȡ�������������ϵ�req�ɷ�??????ΪʲôҪ��������ѽ����һ�����������
      ��reqȫ���ɷ��꣬�ٴ�����һ����������ϵ�req������?ѭ������ֱ������ctx����ϵ�����req���������˳�ѭ����Ӳ������æҲ���˳�
      �һ��Ǿ����е㳶����һ�δ���һ����������ϵ�һ��req��Ȼ����л�����һ��������У�ѭ������������������ʲô��?��˵����Ҳ��
      ��������������ϵ�req??????����ѽ�����nvmeӲ�����з�æ��blk_mq_dispatch_rq_list����false�����˳�ѭ���ˣ������п����е�
      ����������ϵ�reqû�����ü�����ѽ�����������ѭ����?????????????��������������������ô�����???????????
    */

		/*
		 * Now this rq owns the budget which has to be released
		 * if this rq won't be queued to driver via .queue_rq()
		 * in blk_mq_dispatch_rq_list().
		 */
		//req���뵽rq_list
		list_add(&rq->queuelist, &rq_list);

		/* round robin for fair dispatch */
        //ȡ��Ӳ�����й�������һ���������
		ctx = blk_mq_next_ctx(hctx, rq->mq_ctx);

    //����rq_list�ϵ�req���ȸ�req��Ӳ������hctx��blk_mq_tags�����һ������tag������
    //����req��Ӳ�����е���ϵ�ɣ�Ȼ��ֱ������nvmeӲ�����䡣������һ��reqҪ����Ӳ�����䣬��Ҫ��blk_mq_tags�ṹ��õ�һ�����е�tag��
    //���nvmeӲ�����з�æ����Ҫ��rq_listʣ���reqת�Ƶ�hctx->dispatch���У�Ȼ������nvme�첽����
	} while (blk_mq_dispatch_rq_list(q, &rq_list, true));//Ӳ�����з�æ����rq_list������򷵻�flase������ѭ��

    //��ֵhctx->dispatch_from = ctx
	WRITE_ONCE(hctx->dispatch_from, ctx);
}

/* return true if hw queue need to be run again */
//���ָ���������req�ɷ���hctx->dispatchӲ������dispatch�����ϵ�req�ɷ�;��deadline�����㷨ʱ���������fifo���ȶ����ϵ�req�ɷ���
//��IO�����㷨ʱ��Ӳ�����й����������������ctx->rq_list�ϵ�req���ɷ��ȵȡ��ɷ�����Ӧ�ö��ǵ���blk_mq_dispatch_rq_list()��
//nvmeӲ�����в�æֱ������req���䣬��æ�Ļ����ʣ���reqת�Ƶ�hctx->dispatch���У�Ȼ������nvme�첽����
void blk_mq_sched_dispatch_requests(struct blk_mq_hw_ctx *hctx)
{
	struct request_queue *q = hctx->queue;
	struct elevator_queue *e = q->elevator;
	const bool has_sched_dispatch = e && e->aux->ops.mq.dispatch_request;//��IO�����㷨ʱ��__dd_dispatch_request
	LIST_HEAD(rq_list);

	/* RCU or SRCU read lock is needed before checking quiesced flag */
	if (unlikely(blk_mq_hctx_stopped(hctx) || blk_queue_quiesced(q)))
		return;

	hctx->run++;

	/*
	 * If we have previous entries on our dispatch list, grab them first for
	 * more fair dispatch.
	 */
	//���hctx->dispatch����reqҪ�ɷ���
	if (!list_empty_careful(&hctx->dispatch)) {
		spin_lock(&hctx->lock);
        //��hctx->dispatch�����ϵ�reqת�Ƶ�rq_list
		if (!list_empty(&hctx->dispatch))
			list_splice_init(&hctx->dispatch, &rq_list);
		spin_unlock(&hctx->lock);
	}

	/*
	 * Only ask the scheduler for requests, if we didn't have residual
	 * requests from the dispatch list. This is to avoid the case where
	 * we only ever dispatch a fraction of the requests available because
	 * of low device queue depth. Once we pull requests out of the IO
	 * scheduler, we can no longer merge or sort them. So it's best to
	 * leave them there for as long as we can. Mark the hw queue as
	 * needing a restart in that case.
	 *
	 * We want to dispatch from the scheduler if there was nothing
	 * on the dispatch list or we were able to dispatch from the
	 * dispatch list.
	 */
	//���hctx->dispatch����reqҪ�ɷ�
	if (!list_empty(&rq_list)) {
        //���hctx->state��BLK_MQ_S_SCHED_RESTART��־λ
		blk_mq_sched_mark_restart_hctx(hctx);

        //list����hctx->dispatchӲ���ɷ����У�����list�ϵ�req���ȸ�req��Ӳ������hctx��blk_mq_tags�����һ������tag�����ǽ���req��
        //Ӳ�����е���ϵ�ɣ�Ȼ��ֱ������nvmeӲ�����䡣������һ��reqҪ����Ӳ�����䣬��Ҫ��blk_mq_tags�ṹ��õ�һ�����е�tag��
        //���nvmeӲ�����з�æ����Ҫ��listʣ���reqת�Ƶ�hctx->dispatch�������첽����
		if (blk_mq_dispatch_rq_list(q, &rq_list, false)) {
			if (has_sched_dispatch)//�е����㷨
 //ִ��deadline�㷨�ɷ�������ѭ����fifo���ߺ��������ѡ����ɷ��������req��Ȼ���req��Ӳ������hctx��blk_mq_tags�����һ������tag��
 //���ǽ���req��Ӳ�����е���ϵ�ɡ�Ȼ��ֱ������nvmeӲ�����䡣���nvmeӲ�����з�æ�����reqת�Ƶ�hctx->dispatch���У�Ȼ������nvme�첽
 //���䡣Ӳ�����з�æ����deadline�㷨����û��req��������ѭ����
				blk_mq_do_dispatch_sched(hctx);
			else//�޵����㷨
  //����ѭ������hctxӲ�����й���������������У�����ȡ��һ���������ctx->rq_list�ϵ�req����rq_list�ֲ�����ִ��blk_mq_dispatch_rq_list()Ӳ���ɷ�req��
  //���nvmeӲ�����з�æ����Ҫ��rq_listʣ���reqת�Ƶ�hctx->dispatch���У�Ȼ������nvme�첽���䡣ѭ���˳������ǣ�nvmeӲ�����з�æ
  //����hctxӲ�����й�����������������ϵ�reqȫ���ɷ��ꡣ�и����ʣ������nvmeӲ�����з�æ�����п�����Щ��������ϵ�req��û���ü��ɷ�ѽ?????????????
				blk_mq_do_dispatch_ctx(hctx);
		}
	}
    //���hctx->dispatch��û��reqҪ�ɷ�,�����е����㷨
    else if (has_sched_dispatch) {
    //���ϱ�һ����ִ��blk_mq_do_dispatch_sched(),ִ��deadline�㷨�ɷ�������ѭ����fifo���ߺ��������ѡ����ɷ��������reqȥ�ɷ�
		blk_mq_do_dispatch_sched(hctx);
    //Ӳ�����з�æ
	} else if (hctx->dispatch_busy) {
		/* dequeue request one by one from sw queue if queue is busy */
    //���ϱ�һ��������ѭ������hctxӲ�����й���������������У�ȡ��һ����������ϵ�reqȥ�ɷ�
		blk_mq_do_dispatch_ctx(hctx);
	} else {
	  //��Ӳ������hctx��������������ϵ�ctx->rq_list������reqת�Ƶ������rq_list����β����Ȼ�����ctx->rq_list����
      //����ò���ǰ�Ӳ������hctx�����������������ctx->rq_list�����ϵ�reqȫ���ƶ���rq_list����β��ѽ
		blk_mq_flush_busy_ctxs(hctx, &rq_list);
      //����rq_list�ϵ�req���ȸ�req��Ӳ������hctx��blk_mq_tags�����һ������tag������
      //����req��Ӳ�����е���ϵ�ɣ�Ȼ��ֱ������nvmeӲ�����䡣������һ��reqҪ����Ӳ�����䣬��Ҫ��blk_mq_tags�ṹ��õ�һ�����е�tag��
      //���nvmeӲ�����з�æ����Ҫ��rq_listʣ���reqת�Ƶ�hctx->dispatch���У�Ȼ������nvme�첽����
		blk_mq_dispatch_rq_list(q, &rq_list, false);
	}
}

void blk_mq_sched_move_to_dispatch(struct blk_mq_hw_ctx *hctx,
				   struct list_head *rq_list,
				   struct request *(*get_rq)(struct blk_mq_hw_ctx *))
{
	do {
		struct request *rq;

		rq = get_rq(hctx);
		if (!rq)
			break;

		list_add_tail(&rq->queuelist, rq_list);
	} while (1);
}
EXPORT_SYMBOL_GPL(blk_mq_sched_move_to_dispatch);

//��IO����������������Ƿ��п��Ժϲ���req���ҵ������bio�����ǰ��ϲ���req�����ᴥ�����κϲ�������Ժϲ����req��IO�����㷨��������������
bool blk_mq_sched_try_merge(struct request_queue *q, struct bio *bio,
			    struct request **merged_request)
{
	struct request *rq;
	int ret;

//��elv����������������Ƿ��п��Ժϲ���req���ҵ������bio�����ǰ��ϲ���req������ǵ��þ����IO�����㷨����Ѱ�ҿ��Ժϲ���req��
//��������ֵ ELEVATOR_BACK_MERGE(ǰ��ϲ���req)��ELEVATOR_FRONT_MERGE(ǰ��ϲ�)��ELEVATOR_NO_MERGE(û���ҵ����Ժϲ���req)
	ret = elv_merge(q, &rq, bio);
	if (ret == ELEVATOR_BACK_MERGE) {//����ϲ�
		if (!blk_mq_sched_allow_merge(q, rq, bio))
			return false;
        //req��bio���ߴ��̷�Χ���ţ�req���ϲ����ε�bio���ϲ��ɹ�������
		if (bio_attempt_back_merge(q, rq, bio)) {
            //���κϲ�����req��bio�ϲ����µ�req����Ĵ��̽�����ַ��������req������ʼ��ַ�����ˣ��Ǿͽ��ź���ϲ�
			*merged_request = attempt_back_merge(q, rq);
			if (!*merged_request)//���û�з������κϲ������req����deadline�����㷨�������������������
				elv_merged_request(q, rq, ret);
			return true;
		}
	} else if (ret == ELEVATOR_FRONT_MERGE) {//ǰ��ϲ�
		if (!blk_mq_sched_allow_merge(q, rq, bio))
			return false;
        //req��bio���ߴ��̷�Χ���ţ�req��ǰ�ϲ����ε�bio���ϲ��ɹ�������
		if (bio_attempt_front_merge(q, rq, bio)) {
            //���κϲ�����req��bio�ϲ����µ�req����Ĵ��̿ռ���ʼ��ַ��������req�����ˣ��Ǿͽ���ǰ��ϲ�
			*merged_request = attempt_front_merge(q, rq);
			if (!*merged_request)//���û�з������κϲ������req����deadline hash��������������
				elv_merged_request(q, rq, ret);
			return true;
		}
	}

	return false;
}
EXPORT_SYMBOL_GPL(blk_mq_sched_try_merge);

bool __blk_mq_sched_bio_merge(struct request_queue *q, struct bio *bio)
{
	struct elevator_queue *e = q->elevator;

	if (e->aux->ops.mq.bio_merge) {
        //��q->queue_ctx�õ�ÿ��CPUר�����������
		struct blk_mq_ctx *ctx = blk_mq_get_ctx(q);
        //�����������ctx->cpu�󶨵�CPU��ţ�ȥq->queue_hw_ctx[]Ѱ��Ӳ������
		struct blk_mq_hw_ctx *hctx = blk_mq_map_queue(q, ctx->cpu);

		blk_mq_put_ctx(ctx);
//��IO����������������Ƿ��п��Ժϲ���req���ҵ������bio�����ǰ��ϲ���req�����ᴥ�����κϲ�������Ժϲ����req��IO�����㷨��������������
//����ϲ���������к�Ӳ������û�а�ëǮ�Ĺ�ϵ��
		return e->aux->ops.mq.bio_merge(hctx, bio);//mq-deadline�����㷨dd_bio_merge
	}

	return false;
}
//���ȳ��Խ�rq����ϲ���q->last_merge���ٳ��Խ�rq����ϲ���hash���е�ĳһ��__rq���ϲ�������rq��������ʼ��ַ����q->last_merge��__rq
//������������ַ�����ǵ���blk_attempt_req_merge()���кϲ���������IOʹ���ʵ����ݡ����ʹ����deadline�����㷨�����ºϲ����req��
//hash�����е�λ�á������fifo�����޳���rq������dd->next_rq[]��ֵrq����һ��req��
bool blk_mq_sched_try_insert_merge(struct request_queue *q, struct request *rq)
{
	return rq_mergeable(rq) && elv_attempt_insert_merge(q, rq);
}
EXPORT_SYMBOL_GPL(blk_mq_sched_try_insert_merge);

void blk_mq_sched_request_inserted(struct request *rq)
{
	trace_block_rq_insert(rq->q, rq);
}
EXPORT_SYMBOL_GPL(blk_mq_sched_request_inserted);

static bool blk_mq_sched_bypass_insert(struct blk_mq_hw_ctx *hctx,
				       bool has_sched,
				       struct request *rq)
{
	/* dispatch flush rq directly */
	if (rq->cmd_flags & REQ_FLUSH_SEQ) {
		spin_lock(&hctx->lock);
		list_add(&rq->queuelist, &hctx->dispatch);
		spin_unlock(&hctx->lock);
		return true;
	}

	if (has_sched)
		rq->cmd_flags |= REQ_SORTED;

	return false;
}

/**
 * list_for_each_entry_rcu_rr - iterate in a round-robin fashion over rcu list
 * @pos:    loop cursor.
 * @skip:   the list element that will not be examined. Iteration starts at
 *          @skip->next.
 * @head:   head of the list to examine. This list must have at least one
 *          element, namely @skip.
 * @member: name of the list_head structure within typeof(*pos).
 */
#define list_for_each_entry_rcu_rr(pos, skip, head, member)		\
	for ((pos) = (skip);						\
	     (pos = (pos)->member.next != (head) ? list_entry_rcu(	\
			(pos)->member.next, typeof(*pos), member) :	\
	      list_entry_rcu((pos)->member.next->next, typeof(*pos), member)), \
	     (pos) != (skip); )

/*
 * Called after a driver tag has been freed to check whether a hctx needs to
 * be restarted. Restarts @hctx if its tag set is not shared. Restarts hardware
 * queues in a round-robin fashion if the tag set of @hctx is shared with other
 * hardware queues.
 */
void blk_mq_sched_restart(struct blk_mq_hw_ctx *const hctx)
{
	struct blk_mq_tags *const tags = hctx->tags;
	struct blk_mq_tag_set *const set = hctx->queue->tag_set;
	struct request_queue *const queue = hctx->queue, *q;
	struct blk_mq_hw_ctx *hctx2;
	unsigned int i, j;

	if (set->flags & BLK_MQ_F_TAG_SHARED) {
		/*
		 * If this is 0, then we know that no hardware queues
		 * have RESTART marked. We're done.
		 */
		if (!atomic_read(&queue->shared_hctx_restart))
			return;

		rcu_read_lock();
		list_for_each_entry_rcu_rr(q, queue, &set->tag_list,
					   tag_set_list) {
			queue_for_each_hw_ctx(q, hctx2, i)
				if (hctx2->tags == tags &&
				    blk_mq_sched_restart_hctx(hctx2))
					goto done;
		}
		j = hctx->queue_num + 1;
		for (i = 0; i < queue->nr_hw_queues; i++, j++) {
			if (j == queue->nr_hw_queues)
				j = 0;
			hctx2 = queue->queue_hw_ctx[j];
			if (hctx2->tags == tags &&
			    blk_mq_sched_restart_hctx(hctx2))
				break;
		}
done:
		rcu_read_unlock();
	} else {
		blk_mq_sched_restart_hctx(hctx);
	}
}

void blk_mq_sched_insert_request(struct request *rq, bool at_head,
				 bool run_queue, bool async)
{
	struct request_queue *q = rq->q;
	struct elevator_queue *e = q->elevator;
	struct blk_mq_ctx *ctx = rq->mq_ctx;
	struct blk_mq_hw_ctx *hctx = blk_mq_map_queue(q, ctx->cpu);

	/* flush rq in flush machinery need to be dispatched directly */
	if (!(rq->cmd_flags & REQ_FLUSH_SEQ) && (rq->cmd_flags & (REQ_FLUSH | REQ_FUA))) {
		blk_insert_flush(rq);
		goto run;
	}

	WARN_ON(e && (rq->tag != -1));

	if (blk_mq_sched_bypass_insert(hctx, !!e, rq))
		goto run;

	if (e && e->aux->ops.mq.insert_requests) {
		LIST_HEAD(list);

		list_add(&rq->queuelist, &list);
		e->aux->ops.mq.insert_requests(hctx, &list, at_head);
	} else {
		spin_lock(&ctx->lock);
		__blk_mq_insert_request(hctx, rq, at_head);
		spin_unlock(&ctx->lock);
	}

run:
	if (run_queue)
		blk_mq_run_hw_queue(hctx, async);
}
//�����IO�����㷨�����plug->mq_list�����ϵ�req����elv��hash���У�mq-deadline�㷨�Ļ�Ҫ����������fifo���С����û��IO�����㷨����Ӳ������
//����ʱ�����԰�plug->mq_list������ϵ�req������Ӳ������hctx����ϵ�������req���뵽Ӳ������hctx->dispatch���У�ִ��blk_mq_run_hw_queue
//�������reqӲ���ɷ������Ӳ���������ɵ�req�ﵽ���ޣ�Ӳ�����б�æ�����ʣ���plug->mq_list������ϵ�req���뵽�������ctx->rq_list�����ϡ�
void blk_mq_sched_insert_requests(struct request_queue *q,
				  struct blk_mq_ctx *ctx,
				  struct list_head *list, bool run_queue_async)//list��ʱ�����˵�ǰ����plug->mq_list�����ϵĲ���req
{
    //�ҵ�ctx->cpu���CPU��Ŷ�Ӧ��Ӳ�����нṹ
	struct blk_mq_hw_ctx *hctx = blk_mq_map_queue(q, ctx->cpu);
	struct elevator_queue *e = hctx->queue->elevator;//IO�����㷨������

	if (e && e->aux->ops.mq.insert_requests)
        //���Խ�req�ϲ���q->last_merg���ߵ����㷨��hash���е��ٽ�req���ϲ����˵Ļ�����req���뵽deadline�����㷨�ĺ������fifo���У�
        //����req��fifo���еĳ�ʱʱ�䡣������elv�����㷨��hash���С�ע�⣬hash���в���deadline�����㷨���еġ�
		e->aux->ops.mq.insert_requests(hctx, list, false);//mq-deadline�����㷨��������dd_insert_requests

	else {//NONEû�е����㷨��������
		/*
		 * try to issue requests directly if the hw queue isn't
		 * busy in case of 'none' scheduler, and this way may save
		 * us one extra enqueue & dequeue to sw queue.
		 */
		//Ӳ�����в���æ��û��IO�����㷨�������첽����if�ų�����������Ӳ�����з�æʱ����ִ���±ߵĴ���:list�ϵ�req���뵽�������
		if (!hctx->dispatch_busy && !e && !run_queue_async) {
        //���α�����ǰ����plug->mq_list�����ϵ�req������req��Ӳ������hctx����ϵ����������nvmeӲ�����䣬req���������ͳ��IOʹ���ʵ�����
        //����æ�Ļ������req��ӵ�Ӳ������hctx->dispatch���У�ִ��blk_mq_run_hw_queue�������reqӲ���ɷ�
			blk_mq_try_issue_list_directly(hctx, list);
            //list��ʱ�����˵�ǰ����plug->mq_list�����ϵ�req�����list�գ�Ӧ��˵�����е�req���ɷ�Ӳ�����У�Ȼ��Ӳ������Щreq����������˰�?????
			if (list_empty(list))
				return;
		}
        //�����˵��list�����ϻ���ʣ���reqû���ɷ�Ӳ�����д��䡣����Ҫ��Ӳ������û�д����req�����������ѽ!
        
        //��list����ĳ�Ա���뵽��ctx->rq_list�����ߣ�Ȼ���list��0�����list����Դ�Ե�ǰ���̵�plug����ÿһ��req�ڷ���ʱ��
        //req->mq_ctx��ָ��ǰCPU��������У�����������req���뵽������У����ŵ�ִ��blk_mq_insert_requests����ѽ
		blk_mq_insert_requests(hctx, ctx, list);
	}
    
    //����Ӳ��IO�����ɷ�����һ���ص㺯��
	blk_mq_run_hw_queue(hctx, run_queue_async);
}

static void blk_mq_sched_free_tags(struct blk_mq_tag_set *set,
				   struct blk_mq_hw_ctx *hctx,
				   unsigned int hctx_idx)
{
	if (hctx->sched_tags) {
		blk_mq_free_rqs(set, hctx->sched_tags, hctx_idx);
		blk_mq_free_rq_map(hctx->sched_tags);
		hctx->sched_tags = NULL;
	}
}
//ΪӲ�����нṹhctx->sched_tags����blk_mq_tags��һ��Ӳ������һ��blk_mq_tags��Ȼ�����Ϊ���blk_mq_tags����q->nr_requests��request������tags->static_rqs[]
static int blk_mq_sched_alloc_tags(struct request_queue *q,
				   struct blk_mq_hw_ctx *hctx,
				   unsigned int hctx_idx)
{
	struct blk_mq_tag_set *set = q->tag_set;
	int ret;
    //����blk_mq_tags�ṹ�������������Աnr_reserved_tags��nr_tags��rqs��static_rqs
	hctx->sched_tags = blk_mq_alloc_rq_map(set, hctx_idx, q->nr_requests,
					       set->reserved_tags);
	if (!hctx->sched_tags)
		return -ENOMEM;

//���hctx_idx��ŵ�Ӳ�����У�ÿһ�������ȶ�����request(������q->nr_requests��request)��ֵ��tags->static_rqs[]�������Ƿ���N��page����page���ڴ�һƬƬ�ָ��request���ϴ�С
//Ȼ��tags->static_rqs��¼ÿһ��request�׵�ַ��Ȼ��ִ��nvme_init_request()�ײ�������ʼ������,����request��nvme���еĹ�ϵ��
	ret = blk_mq_alloc_rqs(set, hctx->sched_tags, hctx_idx, q->nr_requests);
	if (ret)
		blk_mq_sched_free_tags(set, hctx, hctx_idx);

	return ret;
}

static void blk_mq_sched_tags_teardown(struct request_queue *q)
{
	struct blk_mq_tag_set *set = q->tag_set;
	struct blk_mq_hw_ctx *hctx;
	int i;

	queue_for_each_hw_ctx(q, hctx, i)
		blk_mq_sched_free_tags(set, hctx, i);
}

int blk_mq_sched_init_hctx(struct request_queue *q, struct blk_mq_hw_ctx *hctx,
			   unsigned int hctx_idx)
{
	struct elevator_queue *e = q->elevator;
	int ret;

	if (!e)
		return 0;
    //ΪӲ�����нṹhctx->sched_tags����blk_mq_tags��һ��Ӳ������һ��blk_mq_tags��Ȼ�����Ϊ���blk_mq_tags����q->nr_requests��request������tags->static_rqs[]
	ret = blk_mq_sched_alloc_tags(q, hctx, hctx_idx);
	if (ret)
		return ret;

	if (e->aux->ops.mq.init_hctx) {
		ret = e->aux->ops.mq.init_hctx(hctx, hctx_idx);//nvme_init_hctx
		if (ret) {
			blk_mq_sched_free_tags(q->tag_set, hctx, hctx_idx);
			return ret;
		}
	}

	blk_mq_debugfs_register_sched_hctx(q, hctx);

	return 0;
}

void blk_mq_sched_exit_hctx(struct request_queue *q, struct blk_mq_hw_ctx *hctx,
			    unsigned int hctx_idx)
{
	struct elevator_queue *e = q->elevator;

	if (!e)
		return;

	blk_mq_debugfs_unregister_sched_hctx(hctx);

	if (e->aux->ops.mq.exit_hctx && hctx->sched_data) {
		e->aux->ops.mq.exit_hctx(hctx, hctx_idx);
		hctx->sched_data = NULL;
	}

	blk_mq_sched_free_tags(q->tag_set, hctx, hctx_idx);
}

int blk_mq_init_sched(struct request_queue *q, struct elevator_type *e)
{
	struct blk_mq_hw_ctx *hctx;
	struct elevator_queue *eq;
	unsigned int i;
	int ret;
	struct elevator_type_aux *aux;

	if (!e) {
		q->elevator = NULL;
		q->nr_requests = q->tag_set->queue_depth;
		return 0;
	}

	/*
	 * Default to double of smaller one between hw queue_depth and 128,
	 * since we don't split into sync/async like the old code did.
	 * Additionally, this is a per-hw queue depth.
	 */
	q->nr_requests = 2 * min_t(unsigned int, q->tag_set->queue_depth,
				   BLKDEV_MAX_RQ);

	queue_for_each_hw_ctx(q, hctx, i) {
		ret = blk_mq_sched_alloc_tags(q, hctx, i);
		if (ret)
			goto err;
	}

	aux = elevator_aux_find(e);
	ret = aux->ops.mq.init_sched(q, e);
	if (ret)
		goto err;

	blk_mq_debugfs_register_sched(q);

	queue_for_each_hw_ctx(q, hctx, i) {
		if (aux->ops.mq.init_hctx) {
			ret = aux->ops.mq.init_hctx(hctx, i);
			if (ret) {
				eq = q->elevator;
				blk_mq_exit_sched(q, eq);
				kobject_put(&eq->kobj);
				return ret;
			}
		}
		blk_mq_debugfs_register_sched_hctx(q, hctx);
	}

	return 0;

err:
	blk_mq_sched_tags_teardown(q);
	q->elevator = NULL;
	return ret;
}

void blk_mq_exit_sched(struct request_queue *q, struct elevator_queue *e)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned int i;

	queue_for_each_hw_ctx(q, hctx, i) {
		blk_mq_debugfs_unregister_sched_hctx(hctx);
		if (e->aux->ops.mq.exit_hctx && hctx->sched_data) {
			e->aux->ops.mq.exit_hctx(hctx, i);
			hctx->sched_data = NULL;
		}
	}
	blk_mq_debugfs_unregister_sched(q);
	if (e->aux->ops.mq.exit_sched)
		e->aux->ops.mq.exit_sched(e);
	blk_mq_sched_tags_teardown(q);
	q->elevator = NULL;
}

int blk_mq_sched_init(struct request_queue *q)
{
	int ret;

	mutex_lock(&q->sysfs_lock);
	ret = elevator_init(q, NULL);
	mutex_unlock(&q->sysfs_lock);

	return ret;
}
