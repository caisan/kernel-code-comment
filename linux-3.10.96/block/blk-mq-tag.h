#ifndef INT_BLK_MQ_TAG_H
#define INT_BLK_MQ_TAG_H

#include "blk-mq.h"

/*
 * Tag address space map.
 */
#ifdef __GENKSYMS__
struct blk_mq_tags;
#else

//nvme_dev_add->blk_mq_alloc_tag_set->blk_mq_alloc_rq_maps->__blk_mq_alloc_rq_maps->__blk_mq_alloc_rq_map->blk_mq_alloc_rq_map
//..->blk_mq_init_tags���з���blk_mq_tags���������Աstatic_rqs��rqs��nr_tags��nr_reserved_tags

//ÿ��Ӳ�����ж�Ӧһ��blk_mq_tags��Ӳ�����нṹ��blk_mq_hw_ctx�����߶�����Ӳ�����аɣ��������岻һ����blk_mq_hw_ctx������������
//Ӳ�����У�blk_mq_tags��Ҫ���ڴ�����ȡ��request��,�ǵģ�bioת����reqʱ�����Ǵ�blk_mq_tags��static_rqs[]���������е�req�ɡ�
//blk_mq_hw_ctx�ṹ�ĳ�Աblk_mq_tag_set��tags[]ָ�����鱣��ÿ��Ӳ�����ж��е�blk_mq_tags
struct blk_mq_tags {
	unsigned int nr_tags;//����set->queue_depth��һ��Ӳ�����еĶ�����ȣ���blk_mq_init_tags()
	
	//static_rqs[]����е�request�������±��ƫ�ƣ���blk_mq_get_tag()��������һ����blk_mq_get_driver_tag->blk_mq_tag_is_reserved������
	//nr_reserved_tags��Ԥ����tag���������磬static_rqs[]���鹲��100����Ա��nr_reserved_tags��70���Ǿ���Ԥ��70����Ԥ���ķ������ˣ��Ǿʹ�
	//ʣ���30������?????��Щreq���±���70+0/1/2/3�ȡ�static_rqs[tag]�����±���tag������ʾ��һ��reqһ��tag���ܹؼ�!!!!!
	unsigned int nr_reserved_tags;//blk_mq_init_tags()�з���

	atomic_t active_queues;
    
    //���bitmap_tags��Ӧ��������ʶstatic_rqs[]�������ĸ�request������ʹ���ˣ���������һ��bitλ0/1��ʶ��request�Ƿ񱻷�����
    //blk_mq_put_tagȥ��tag��blk_mq_get_tag()��ȡtag
	struct sbitmap_queue bitmap_tags;//blk_mq_init_tags->blk_mq_init_bitmap_tags �з��䡣�޵�������req����tagʹ�á�
	struct sbitmap_queue breserved_tags;//blk_mq_init_tags->blk_mq_init_bitmap_tags �з��䡣ʹ�õ�������req����tagʹ�á�

/*�ڷ���reqʱ��Ҫ��blk_mq_tags�����һ��tag����blk_mq_alloc_rqs������reqӲ������ǰ��ҲҪ��blk_mq_tags�����һ������tag����
blk_mq_get_driver_tag������󶼵���blk_mq_get_tag��bitmap_tags�õ�һ������bit������Ŀ��е�tag���б�Ҫִ��������ɶ��˼
?????????????????????????????*/

    //��blk_mq_get_driver_tag()->blk_mq_get_tag��hctx->tags->rqs[req->tag]=req��req���Խ��̵�plug->mq_list����
    //��ֵ��ͽ�����req��Ӳ�����еĹ�ϵ�����ﱣ���static_rqs����õ���req��
	struct request **rqs;//��__blk_mq_alloc_request()����߱����req�Ǹմ�static_rqs[]�õ��Ŀ��е�req

    /*static_rqs[]���req����Ӳ������ͬʱ���֧�ֵģ�bioתreqʱ��Ҫ��static_rqs[]���Եõ�һ�����еģ��ò�����Ҫ���ߵȴ�
     nvmeӲ���������е�req��������ɺ��ͷŵ���static_rqs[]���п��е�req���Է����ˡ���blk_mq_get_tag()
     */
	//static_rqsָ�����飬������һ����Ա����ÿһ�������ȶ�Ӧ��request�ṹ�׵�ַ��Ӳ������ÿһ����ȣ���Ӧһ��request�ṹ��
	//������̼�__blk_mq_alloc_rq_map->blk_mq_alloc_rqs��ʹ�ù��̼�blk_mq_get_tag(),����ϸ���ϱ�nr_reserved_tags������ע�͡�
	struct request **static_rqs;//bio��Ҫת����req,��static_rqsȡ��req

	//blk_mq_alloc_rqs()�з���page,Ȼ����ӵ�page_list��
	struct list_head page_list;
};
#endif


extern struct blk_mq_tags *blk_mq_init_tags(unsigned int nr_tags, unsigned int reserved_tags, int node, int alloc_policy);
extern void blk_mq_free_tags(struct blk_mq_tags *tags);

extern unsigned int blk_mq_get_tag(struct blk_mq_alloc_data *data);
extern void blk_mq_put_tag(struct blk_mq_hw_ctx *hctx, struct blk_mq_tags *tags,
			   struct blk_mq_ctx *ctx, unsigned int tag);
extern bool blk_mq_has_free_tags(struct blk_mq_tags *tags);
extern int blk_mq_tag_update_depth(struct blk_mq_hw_ctx *hctx,
					struct blk_mq_tags **tags,
					unsigned int depth, bool can_grow);
extern void blk_mq_tag_wakeup_all(struct blk_mq_tags *tags, bool);
void blk_mq_queue_tag_busy_iter(struct request_queue *q, busy_iter_fn *fn,
		void *priv);

static inline struct sbq_wait_state *bt_wait_ptr(struct sbitmap_queue *bt,
						 struct blk_mq_hw_ctx *hctx)
{
	if (!hctx)
		return &bt->ws[0];
	return sbq_wait_ptr(bt, &hctx->wait_index);
}

enum {
	BLK_MQ_TAG_CACHE_MIN	= 1,
	BLK_MQ_TAG_CACHE_MAX	= 64,
};

enum {
	BLK_MQ_TAG_FAIL		= -1U,
	BLK_MQ_TAG_MIN		= BLK_MQ_TAG_CACHE_MIN,
	BLK_MQ_TAG_MAX		= BLK_MQ_TAG_FAIL - 1,
};

extern bool __blk_mq_tag_busy(struct blk_mq_hw_ctx *);
extern void __blk_mq_tag_idle(struct blk_mq_hw_ctx *);

static inline bool blk_mq_tag_busy(struct blk_mq_hw_ctx *hctx)
{
    //û�����ù���tag��־����false
	if (!(hctx->flags & BLK_MQ_F_TAG_SHARED))
		return false;
    
    //���򷵻�true
	return __blk_mq_tag_busy(hctx);
}

static inline void blk_mq_tag_idle(struct blk_mq_hw_ctx *hctx)
{
	if (!(hctx->flags & BLK_MQ_F_TAG_SHARED))
		return;

	__blk_mq_tag_idle(hctx);
}

/*
 * This helper should only be used for flush request to share tag
 * with the request cloned from, and both the two requests can't be
 * in flight at the same time. The caller has to make sure the tag
 * can't be freed.
 */
static inline void blk_mq_tag_set_rq(struct blk_mq_hw_ctx *hctx,
		unsigned int tag, struct request *rq)
{
	hctx->tags->rqs[tag] = rq;
}

static inline bool blk_mq_tag_is_reserved(struct blk_mq_tags *tags,
					  unsigned int tag)
{
	return tag < tags->nr_reserved_tags;
}

#endif
