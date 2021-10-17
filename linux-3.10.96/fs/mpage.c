/*
 * fs/mpage.c
 *
 * Copyright (C) 2002, Linus Torvalds.
 *
 * Contains functions related to preparing and submitting BIOs which contain
 * multiple pagecache pages.
 *
 * 15May2002	Andrew Morton
 *		Initial version
 * 27Jun2002	axboe@suse.de
 *		use bio_add_page() to build bio's just the right size
 */

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/kdev_t.h>
#include <linux/gfp.h>
#include <linux/bio.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/prefetch.h>
#include <linux/mpage.h>
#include <linux/writeback.h>
#include <linux/backing-dev.h>
#include <linux/pagevec.h>
#include <linux/cleancache.h>

/*
 * I/O completion handler for multipage BIOs.
 *
 * The mpage code never puts partial pages into a BIO (except for end-of-file).
 * If a page does not map to a contiguous run of blocks then it simply falls
 * back to block_read_full_page().
 *
 * Why is this?  If a page's completion depends on a number of different BIOs
 * which can complete in any order (or at the same time) then determining the
 * status of that page is hard.  See end_buffer_async_read() for the details.
 * There is no point in duplicating all that complexity.
 */
//blk_update_request->bio_endio->mpage_end_io
static void mpage_end_io(struct bio *bio, int err)
{
	const int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	struct bio_vec *bvec = bio->bi_io_vec + bio->bi_vcnt - 1;

	do {
		struct page *page = bvec->bv_page;

		if (--bvec >= bio->bi_io_vec)
			prefetchw(&bvec->bv_page->flags);
		if (bio_data_dir(bio) == READ) {//���ļ�
			if (uptodate) {
				SetPageUptodate(page);//����page��"PageUptodate"״̬
			} else {//�����ֻ֧�г���ʱ�ų���
				ClearPageUptodate(page);
				SetPageError(page);
			}
			unlock_page(page);
		} else { /* bio_data_dir(bio) == WRITE */ //д�ļ�
			if (!uptodate) {
				SetPageError(page);
				if (page->mapping)
					set_bit(AS_EIO, &page->mapping->flags);
			}
			end_page_writeback(page);
		}
	} while (bvec >= bio->bi_io_vec);
	bio_put(bio);
}

static struct bio *mpage_bio_submit(int rw, struct bio *bio)
{
	bio->bi_end_io = mpage_end_io;
	submit_bio(rw, bio);
	return NULL;
}

static struct bio *
mpage_alloc(struct block_device *bdev,
		sector_t first_sector, int nr_vecs,
		gfp_t gfp_flags)
{
	struct bio *bio;

	bio = bio_alloc(gfp_flags, nr_vecs);

	if (bio == NULL && (current->flags & PF_MEMALLOC)) {
		while (!bio && (nr_vecs /= 2))
			bio = bio_alloc(gfp_flags, nr_vecs);
	}

	if (bio) {
		bio->bi_bdev = bdev;
		bio->bi_sector = first_sector;
	}
	return bio;
}

/*
 * support function for mpage_readpages.  The fs supplied get_block might
 * return an up to date buffer.  This is used to map that buffer into
 * the page, which allows readpage to avoid triggering a duplicate call
 * to get_block.
 *
 * The idea is to avoid adding buffers to pages that don't already have
 * them.  So when the buffer is up to date and the page size == block size,
 * this marks the page up to date instead of adding new buffers.
 */
static void 
map_buffer_to_page(struct page *page, struct buffer_head *bh, int page_block) 
{
	struct inode *inode = page->mapping->host;
	struct buffer_head *page_bh, *head;
	int block = 0;

	if (!page_has_buffers(page)) {
		/*
		 * don't make any buffers if there is only one buffer on
		 * the page and the page just needs to be set up to date
		 */
		if (inode->i_blkbits == PAGE_CACHE_SHIFT && 
		    buffer_uptodate(bh)) {
			SetPageUptodate(page);    
			return;
		}
		create_empty_buffers(page, 1 << inode->i_blkbits, 0);
	}
	head = page_buffers(page);
	page_bh = head;
	do {
		if (block == page_block) {
			page_bh->b_state = bh->b_state;
			page_bh->b_bdev = bh->b_bdev;
			page_bh->b_blocknr = bh->b_blocknr;
			break;
		}
		page_bh = page_bh->b_this_page;
		block++;
	} while (page_bh != head);
}

/*
 * This is the worker routine which does all the work of mapping the disk
 * blocks and constructs largest possible bios, submits them for IO if the
 * blocks are not contiguous on the disk.
 *
 * We pass a buffer_head back and forth and use its buffer_mapped() flag to
 * represent the validity of its disk mapping and to decide when to do the next
 * get_block() call.
 */
 
/*
1 ÿ��ִ��do_mpage_readpage���һ��page�ļ�ҳ�����������ӳ��
2 �����������Ĵ����������ִ��submit_bio�������ϵ�bio��֮���·���һ��bio

mpage_readpages->do_mpage_readpage ����ʵ�ʲ���Ϊ��

cat�����ļ�Ԥ�����ļ���С64*4K�����ִ�е�mpage_readpages������ѭ��ִ��64��do_mpage_readpage��ÿ��ѭ����ȡ1���ļ�ҳpage��
������������ӣ�1:��64*4k���ļ����������ķֲ��ڴ��̵�ַ��0~64*4K��2:��64*4K���ļ����������ķֲ��ڴ��̵�ַ��0~32*4k��64*4K~96*4k

mpage_readpages cat 17648 nr_pages:64
do_mpage_readpage cat 17648 nr_pages:64 page:0xffffd91e02a542c0 page->index:0 last_block_in_bio:0 first_logical_block:0 map_bh->b_blocknr:0 map_bh->b_page:          (null) map_bh->b_size:0 map_bh->b_state:0x0

����ǰ����Ҫ˵��һ�㣬���Ա���ext4�ļ�ϵͳһ������������С��4K��inode->i_blkbits=12��super_block->s_blocksize_bits=12����ˣ�
һ���ļ�ҳpageֻ��Ӧһ�����������


************���1:64*4K���ļ����������ķֲ��ڴ��̵�ַ��0~64*4K**************************************

��1��ִ��do_mpage_readpage()....................................
1 ��1��ִ��do_mpage_readpage()�����1��page�ļ�ҳ�Ĵ�������飬
  if (buffer_mapped(map_bh) && block_in_file > *first_logical_block...)��������while (page_block < blocks_per_page)������
  ִ��if (get_block(inode, block_in_file, map_bh, 0))����ext4_get_block()���ú�����������
  
      ���ݱ��ζ�д���ļ���ʼ�����߼����block_in_file������ȡ���ļ�������bh->b_size������ļ������߼���ַ��һƬ�����Ĵ���������
      ӳ�䡣�������غ�bh->b_blocknr��ӳ��Ĵ���������ַ��bh->b_size��ʵ�����ӳ��Ĵ������������*���С����������bh->b_state
      ��"mapped"״̬��buffer_mapped(map_bh)�������
      ע�⣬���ζ�ȡ�ļ������߼���ַ������һ��ȫ�����ӳ�䡣�����ļ�64*4K��С��������64*4K������������̷ֳ����飬����������ַ0~32*4k��
      64*4k~96*4k�����һ��ִ��_ext4_get_block����ļ���ַ0~32*4K����������0~32*4k��ӳ�䣬֮���֪�����ļ���ַ0~32*4K��Ӧ�Ĵ��������
      ��ַ��ִ��submit_bio���ļ����ݴ��䵽��Ӧ���������(����д�����򷴹���)������ִ��_ext4_get_block����ļ���ַ32*4~64*4K��
      ���������64*4k~96*4k��ӳ�䣬���ͬ��ִ��submit_bio���ļ����ݴ��䵽��Ӧ���������(����д�����򷴹���)����Ȼ������ļ�64*4K����
      �ڴ���������������ֲ�����ִ��һ��ext4_get_block()���ܵõ��ļ�0~64*4k�����ڴ���������ַ��������ִ��submit_bio���ļ�
      ���ݴ��䵽��Ӧ���������(����д�����򷴹���)

  ִ��ext4_get_block()��bh->b_blocknr=4295648(�ļ�ӳ��ĵ�һ������������ַ)��bh->b_size=64*4k��bh->b_state��mapped��ǣ�
  first_logical_blockʼ����0��blocks_per_pageʼ����1��map_bh->b_sizeʼ��64*4K��nblocks=64
  block_in_file=0��page->index=0��

  �ڸ�while (page_block < blocks_per_page)�ִ����ߵ�
  for (relative_block = 0; ; relative_block++) {}���blocks[page_block=0] = map_bh->b_blocknr + relative_block=4295648��
  ��һ��forѭ����else if (page_block == blocks_per_page)����break

2  222�е�if (bio && (*last_block_in_bio != blocks[0] - 1))������

3 ִ�� bio = mpage_alloc(bdev, blocks[0] << (blkbits - 9)..)�����µ�bio,��bio->bi_sector=4295648��¼�ļ�ӳ��ĵ�һ������������ַ

4 ִ��if (bio_add_page(bio, page, length, 0) < length)��Ϊ����Ҫ����ĵ�1���ļ�ҳ����page����һ��bio_vec�ṹ����¼�ļ�����page
  �ڴ��ַ���ļ�ҳ���ݴ�С��Ȼ���bio_vec��ӵ�bio��

5 ִ�� *last_block_in_bio = blocks[blocks_per_page - 1]����ʵ����*last_block_in_bio = blocks[0]=4295648,��last_block_in_bio
  ��ֵΪ���ļ�ҳӳ��ĵ�һ������������ַ��
  
  ext4�ļ�ϵͳһ�����������4K����һ��page�ļ�ҳֻ��Ӧһ����������飬blocks_per_page=1.����do_mpage_readpage()ִ��һ�δ���һ��
  �ļ�ҳpage,ֻ�õ�һ���ļ�ҳpageӳ��Ĵ��������ĵ�ַ��

6 return bio

��2��ִ��do_mpage_readpage()....................................
1 ��2��ִ�� do_mpage_readpage�������2��page�Ĵ�������飬if (buffer_mapped(map_bh) && block_in_file > *first_logical_block...)
  ������first_logical_blockʼ����0��page->index��1��block_in_file��1��
  ִ����ߵ�for (relative_block = 0; ; relative_block++){}��blocks[page_block=0] =map_bh->b_blocknr+map_offset(1)+relative_block=4295648+1
  ,������blocks[0]��¼�ļ�ӳ��ĵڶ�������������ַ��Ȼ��page_block++
  
2 while (page_block < blocks_per_page) ������
3 ִ��if (bio_add_page(bio, page, length, 0) < length)��Ϊ����Ҫ����ĵ�2���ļ�ҳ����page����һ��bio_vec�ṹ����¼�ļ�����page
  �ڴ��ַ���ļ�ҳ���ݴ�С��Ȼ���bio_vec��ӵ�bio��
  
4 ִ�� *last_block_in_bio = blocks[blocks_per_page - 1]����ʵ����*last_block_in_bio = blocks[0]=4295648+1,��last_block_in_bio
  ��ֵΪ���ļ�ӳ��ĵ�2������������ַ
5 return bio

.....ʡ��.......

��64��ִ��do_mpage_readpage()....................................
1 ��64��ִ�� do_mpage_readpage�������64��page�Ĵ�������飬if (buffer_mapped(map_bh) && block_in_file > *first_logical_block...)
  ������first_logical_blockʼ����0��page->index��63��block_in_file��63��
  ִ����ߵ�for (relative_block = 0; ; relative_block++){}��blocks[page_block=0] =map_bh->b_blocknr+map_offset(63)+relative_block=4295648+63
  ,������blocks[0]��¼�ļ�ӳ��ĵ�64������������ַ��Ȼ��page_block++
  
2 while (page_block < blocks_per_page) ������
3 ִ��if (bio_add_page(bio, page, length, 0) < length)��Ϊ����Ҫ����ĵ�64���ļ�ҳ����page����һ��bio_vec�ṹ����¼�ļ�����page
  �ڴ��ַ���ļ�ҳ���ݴ�С��Ȼ���bio_vec��ӵ�bio��
  
4 ִ�� *last_block_in_bio = blocks[blocks_per_page - 1]����ʵ����*last_block_in_bio = blocks[0]=4295648+63,��last_block_in_bio
  ��ֵΪ���ļ�ӳ��ĵ�64������������ַ

5 return bio�ص�mpage_readpages()����mpage_readpages�������ִ��mpage_bio_submit(READ, bio)->submit_bio()��bio���͸���������
6 ע�⣬��1�ε���64ִ��do_mpage_readpage()���ΰ�64��page����ӵ�bio�����bioʼ����ͬһ������mpage_bio_submit(READ, bio)���͸�bio��
  ����bio=NULL,��bioʧЧ��֮���ַ���һ��bio








********���2:64*4k���ļ����ݷֲ���������̵�ַ0~32*4k��64*4k~96*4k ������*******************************************
���������64*4k�ļ����ݲ����������ֲ���������̣���������仯

��1��ִ��do_mpage_readpage()....................................
1 ��1��ִ��do_mpage_readpage()�����1��page�ļ�ҳ�Ĵ�������飬
  if (buffer_mapped(map_bh) && block_in_file > *first_logical_block...)��������while (page_block < blocks_per_page)������
  ִ��if (get_block(inode, block_in_file, map_bh, 0))����ext4_get_block()���ú�����������
  
      ���ݱ��ζ�д���ļ���ʼ�����߼����block_in_file������ȡ���ļ�������bh->b_size������ļ������߼���ַ��һƬ�����Ĵ���������
      ӳ�䡣�������غ�bh->b_blocknr��ӳ��Ĵ���������ַ��bh->b_size��ʵ�����ӳ��Ĵ������������*���С����������bh->b_state
      ��"mapped"״̬��buffer_mapped(map_bh)�������
      ע�⣬���ζ�ȡ�ļ������߼���ַ������һ��ȫ�����ӳ�䡣�����ļ�64*4K��С��������64*4K������������̷ֳ����飬����������ַ0~32*4k��
      64*4k~96*4k�����һ��ִ��_ext4_get_block����ļ���ַ0~32*4K����������0~32*4k��ӳ�䣬֮���֪�����ļ���ַ0~32*4K��Ӧ�Ĵ��������
      ��ַ��ִ��submit_bio���ļ����ݴ��䵽��Ӧ���������(����д�����򷴹���)������ִ��_ext4_get_block����ļ���ַ32*4~64*4K��
      ���������64*4k~96*4k��ӳ�䣬���ͬ��ִ��submit_bio���ļ����ݴ��䵽��Ӧ���������(����д�����򷴹���)����Ȼ������ļ�64*4K����
      �ڴ���������������ֲ�����ִ��һ��ext4_get_block()���ܵõ��ļ�0~64*4k�����ڴ���������ַ��������ִ��submit_bio���ļ�
      ���ݴ��䵽��Ӧ���������(����д�����򷴹���)

  -----����ͷ����˱仯��ִ��ext4_get_block()ֻӳ�����ļ��߼���ַ0~32*4K��������̵�ַ0~32*4k��ӳ�䣬ֻӳ�����ļ�ǰ32�����������
  
  ִ��ext4_get_block()��bh->b_blocknr=4295648(�ļ�ӳ��ĵ�һ������������ַ)��bh->b_size=32*4k��bh->b_state��mapped��ǣ�
  first_logical_block��0��blocks_per_pageʼ����1��nblocks=32
  block_in_file=0��page->index=0��

  �ڸ�while (page_block < blocks_per_page)�ִ����ߵ�
  for (relative_block = 0; ; relative_block++) {}���blocks[page_block=0] = map_bh->b_blocknr + relative_block=4295648��
  ��һ��forѭ����else if (page_block == blocks_per_page)����break
  
2 ִ�� bio = mpage_alloc(bdev, blocks[0] << (blkbits - 9)..)�����µ�bio,��bio->bi_sector=4295648��¼�ļ�ӳ��ĵ�һ������������ַ

3 ִ��if (bio_add_page(bio, page, length, 0) < length)��Ϊ����Ҫ����ĵ�1���ļ�ҳ����page����һ��bio_vec�ṹ����¼�ļ�����page
  �ڴ��ַ���ļ�ҳ���ݴ�С��Ȼ���bio_vec��ӵ�bio��

4 ִ�� *last_block_in_bio = blocks[blocks_per_page - 1]����ʵ����*last_block_in_bio = blocks[0]=4295648,��last_block_in_bio
  ��ֵΪ���ļ�ӳ��ĵ�һ������������ַ��
  
  ext4�ļ�ϵͳһ�����������4K����һ��page�ļ�ҳֻ��Ӧһ����������飬blocks_per_page=1.����do_mpage_readpage()ִ��һ�δ���һ��
  �ļ�ҳpage,ֻ�õ�һ���ļ�ҳpageӳ��Ĵ��������ĵ�ַ��
  
5 return bio

....................................


��2��ִ��do_mpage_readpage()....................................
1 ��2��ִ�� do_mpage_readpage�������2��page�Ĵ�������飬if (buffer_mapped(map_bh) && block_in_file > *first_logical_block...)
  ������first_logical_blockʼ����0��page->index��1��block_in_file��1
  ִ����ߵ�for (relative_block = 0; ; relative_block++){}��blocks[page_block=0] =map_bh->b_blocknr+map_offset(1)+relative_block=4295648+1
  ,������blocks[0]��¼�ļ�ӳ��ĵڶ�������������ַ,Ȼ��page_block++
  
2 while (page_block < blocks_per_page) ������
3 ִ��if (bio_add_page(bio, page, length, 0) < length)��Ϊ����Ҫ����ĵ�2���ļ�ҳ����page����һ��bio_vec�ṹ����¼�ļ�����page
  �ڴ��ַ���ļ�ҳ���ݴ�С��Ȼ���bio_vec��ӵ�bio��
  
4 ִ�� *last_block_in_bio = blocks[blocks_per_page - 1]����ʵ����*last_block_in_bio = blocks[0]=4295648+1,��last_block_in_bio
  ��ֵΪ���ļ�ӳ��ĵ�2������������ַ
5 return bio

��2~32��ִ��do_mpage_readpage()�������ǰ��һ������33�η����仯


��33��ִ��do_mpage_readpage()....................................
1 ��33��ִ�� do_mpage_readpage�������33��page�Ĵ�������飬if (buffer_mapped(map_bh) && block_in_file > *first_logical_block...)
  ������first_logical_block��ʱ����0��page->index��32��block_in_file��32��nblocks=32��page_block=0��
  map_offset =block_in_file-*first_logical_block=32��last=nblocks -map_offset=0��if (relative_block == last)������ֱ��break��

  ��ʱpage_block=0��while (page_block < blocks_per_page)������ִ��if (get_block(inode, block_in_file, map_bh, 0))����ext4_get_block()��
  ����ļ��߼���ַ32*4K~64*4K��������̵�ַ64*4k~96k��ӳ�䣬
  ִ��ext4_get_block()��bh->b_blocknr=4295648+64��bh->b_size=32*4k��bh->b_state��mapped��ǡ��ص����ˣ�
  *first_logical_block = block_in_file=32��first_logical_block֮��һֱ��32��blocks_per_pageʼ����1��map_bh->b_sizeʼ��32*4K��nblocks=32��

  map_bh->b_size ��ʾ���һ���ļ��߼���ַӳ����������������ռ��С

  �ڸ�while (page_block < blocks_per_page)�ִ����ߵ�
  for (relative_block = 0; ; relative_block++) {}���blocks[page_block=0] = map_bh->b_blocknr + relative_block=4295648+64��
  ��һ��forѭ����else if (page_block == blocks_per_page)����break

2 if (bio && (*last_block_in_bio != blocks[0] - 1))������*last_block_in_bio=4295648+32��blocks[0] - 1 = 4295648+64-1 ��������
  bio = mpage_bio_submit(READ, bio)�Ѹ�bio���͵ô���������Ȼ���bio=NULL.
  ����˵��bio������һƬ�����Ĵ�������죬����������������Ǿ�Ҫ������bio���͸�����������
  
2 bioΪNULL��ִ�� bio = mpage_alloc(bdev, blocks[0] << (blkbits - 9)..)�����µ�bio,��bio->bi_sector=4295648+64
  ��¼�ļ�ӳ��ĵ�33������������ַ

3 ִ��if (bio_add_page(bio, page, length, 0) < length)��Ϊ����Ҫ����ĵ�33���ļ�ҳ����page����һ��bio_vec�ṹ����¼�ļ�����page
  �ڴ��ַ���ļ�ҳ���ݴ�С��Ȼ���bio_vec��ӵ�bio��

4 ִ�� *last_block_in_bio = blocks[blocks_per_page - 1]����ʵ����*last_block_in_bio = blocks[0]=4295648+64,��last_block_in_bio
  ��ֵΪ���ļ�ӳ��ĵ�33������������ַ��
  
  ext4�ļ�ϵͳһ�����������4K����һ��page�ļ�ҳֻ��Ӧһ����������飬blocks_per_page=1.����do_mpage_readpage()ִ��һ�δ���һ��
  �ļ�ҳpage,ֻ�õ�һ���ļ�ҳpageӳ��Ĵ��������ĵ�ַ��
  
5 return bio

��34��ִ��do_mpage_readpage()....................................
1 ��34��ִ�� do_mpage_readpage�������34��page�Ĵ�������飬if (buffer_mapped(map_bh) && block_in_file > *first_logical_block...)
  ������first_logical_block��32��page->index��33��block_in_file��33
  ִ����ߵ�for (relative_block = 0; ; relative_block++){}��
  blocks[page_block=0] =map_bh->b_blocknr+map_offset(1)+relative_block=4295648+64+1��Ȼ��page_block++
  ,������blocks[0]��¼�ļ�ӳ��ĵ�34������������ַ
  
2 while (page_block < blocks_per_page) ������
3 ִ��if (bio_add_page(bio, page, length, 0) < length)��Ϊ����Ҫ����ĵ�2���ļ�ҳ����page����һ��bio_vec�ṹ����¼�ļ�����page
  �ڴ��ַ���ļ�ҳ���ݴ�С��Ȼ���bio_vec��ӵ�bio��
  
4 ִ�� *last_block_in_bio = blocks[blocks_per_page - 1]����ʵ����*last_block_in_bio = blocks[0]=4295648+64+1,��last_block_in_bio
  ��ֵΪ���ļ�ӳ��ĵ�34������������ַ
5 return bio

.......34~63��һ��............

��64��ִ��do_mpage_readpage()
1 ��64��ִ�� do_mpage_readpage�������64��page�Ĵ�������飬if (buffer_mapped(map_bh) && block_in_file > *first_logical_block...)
  ������first_logical_blockʼ����0��page->index��63��block_in_file��63
  ִ����ߵ�for (relative_block = 0; ; relative_block++){}��
  blocks[page_block=0] =map_bh->b_blocknr+map_offset(63)+relative_block=4295648+64+31
  ,������blocks[0]��¼�ļ�ӳ��ĵڶ�������������ַ��Ȼ��page_block++
  
2 while (page_block < blocks_per_page) ������
3 ִ��if (bio_add_page(bio, page, length, 0) < length)��Ϊ����Ҫ����ĵ�64���ļ�ҳ����page����һ��bio_vec�ṹ����¼�ļ�����page
  �ڴ��ַ���ļ�ҳ���ݴ�С��Ȼ���bio_vec��ӵ�bio��
  
4 ִ�� *last_block_in_bio = blocks[blocks_per_page - 1]����ʵ����*last_block_in_bio = blocks[0]=4295648+64+31,��last_block_in_bio
  ��ֵΪ���ļ�ӳ��ĵ�64������������ַ

5 return bio�ص�mpage_readpages()����mpage_readpages�������ִ��mpage_bio_submit(READ, bio)->submit_bio()��bio���͸���������
6 ע�⣬����һ���԰�32��page����ӵ�bio�����bioʼ����ͬһ������mpage_bio_submit(READ, bio)���͸�bio��
  ����bio=NULL,��bioʧЧ��֮���ַ���һ��bio

ok�����ϰ��ļ��߼���ַ��һƬ�����Ĵ��������ַ����ӳ�䡢�ļ��߼���ַ�벻�����Ĵ��������ַ����ӳ�䣬��ôֻ�ǰ��ļ����ݴ��䵽���̻���
�Ӵ��̶����ݽ����ˡ�

�и����֣��е�΢���ֵĸ������ļ��߼���ַ��һƬ�����Ĵ��������ַ����ӳ�䣬����ext4_get_block�����
���ļ�ӳ������д���������ַ��ֻ����һ��bio�ṹ���Ѹ��ļ���4K�ļ�ҳpageΪ��λ����bio��¼ÿһ���ļ�ҳpage��ַ���ļ�ҳpageӳ���
����������ַ�����bio��¼ÿһ���ļ�ҳpage��ַ���ļ�ҳpageӳ��Ĵ���������ַ��submit_bio���Ѹ�bio���͸��������������ļ�ҳpage
�����ݷ��͸���Ӧ�Ĵ���������ַ������д������Ƕ���������ִ��submit-bio��page�ļ�ҳ��Ӧ�Ĵ���������ַ��ȡ���ݵ�page�ļ�ҳ��

����ļ��߼���ַ���Ƭ�����Ĵ��������ַ����ӳ�䣬�����ext4_get_block���������һƬ������������ļ��߼���ַ��ӳ���ϵ��֪������Ƭ
�ļ��߼���ַӳ��Ĵ���������ַ������һ��bio�ṹ����¼��Ƭ�ļ��߼���ַÿһ���ļ�ҳpage��ַ���ļ�ҳpageӳ��Ĵ���������ַ��������
submit_bio��bio���͸�����������

Ȼ�󣬵���ext4_get_block���������2Ƭ������������ļ��߼���ַ��ӳ���ϵ��֪������Ƭ
�ļ��߼���ַӳ��Ĵ���������ַ���ٷ���һ��bio�ṹ����¼��Ƭ�ļ��߼���ַÿһ���ļ�ҳpage��ַ���ļ�ҳpageӳ��Ĵ���������ַ��������
submit_bio��bio���͸���������
.....�ظ�
*/
static struct bio *
do_mpage_readpage(struct bio *bio, struct page *page, unsigned nr_pages,//nr_pages:64
		sector_t *last_block_in_bio, struct buffer_head *map_bh,
		unsigned long *first_logical_block, get_block_t get_block)
{
	struct inode *inode = page->mapping->host;
	const unsigned blkbits = inode->i_blkbits;//inode->i_blkbits��ӡ��12���һ���Ϊ��10
	
    //һ���ļ�ҳpage��4K�ڴ���Ա�����ٸ������������ļ����ݡ�inode->i_blkbits��Ϊ10��һ��page��Ӧ4����������顣
    //inode->i_blkbits��Ϊ12��1��page��Ӧ1�����������
	const unsigned blocks_per_page = PAGE_CACHE_SIZE >> blkbits;
    //����������С��inode->i_blkbitsΪ10ʱ��1K��inode->i_blkbitsΪ12ʱ��4K
	const unsigned blocksize = 1 << blkbits;
	sector_t block_in_file;
	sector_t last_block;
	sector_t last_block_in_file;
	sector_t blocks[MAX_BUF_PER_PAGE];
	unsigned page_block;
	unsigned first_hole = blocks_per_page;
	struct block_device *bdev = NULL;
	int length;
	int fully_mapped = 1;
	unsigned nblocks;
	unsigned relative_block;

	if (page_has_buffers(page))
		goto confused;
    //���ζ�ȡ��page�ļ�ҳ��Ӧ����ʼ�����߼���ţ����������Ĵ���������ַ���Ǹ���Ե�ַ������page->index��0����������������ʼ����
    //�߼������0�����һ��page��Ӧ4����������飬������pageҳ��������4�������Ӧ��ʼ�����߼����
	block_in_file = (sector_t)page->index << (PAGE_CACHE_SHIFT - blkbits);
    //���ֶ�ȡ���ļ�������ַ�Ĵ����߼���ţ����Ǵ��������š�ʵ�ʲ���ʱ��ִ��do_mpage_readpage��ȡ�ļ�ǰ64*4k��ַ�����ݣ�last_blockʼ����64
	last_block = block_in_file + nr_pages * blocks_per_page;
    
    //�ļ�������ַ��Ӧ�Ĵ����߼����
	last_block_in_file = (i_size_read(inode) + blocksize - 1) >> blkbits;
	if (last_block > last_block_in_file)
		last_block = last_block_in_file;
	page_block = 0;

    /*����ʱ�ļ�64*4K=256K��С��������64*4K���������ķֲ���������̿�*/
    
	/*
	 * Map blocks using the result from the previous get_blocks call first.
	 */
	//��һ��ִ��mpage_readpages->do_mpage_readpage��map_bh->b_size��0�����±�map_bh->b_size����ֵ(last_block-block_in_file) << blkbits
	//���һ���map_bh���ظ�mpage_readpages�������ִ��mpage_readpages->do_mpage_readpage��map_bh��ΪNULL�����ҵ�һ��ִ��do_mpage_readpage
	//->ext4_get_block�����map_bhӳ�䣬buffer_mapped(map_bh)����TRUE��first_logical_blockʼ����0��nblocks=64�����if����
	nblocks = map_bh->b_size >> blkbits;//map_bh->b_size ��ʾ�ļ��߼���ַ����������߼���ַӳ��Ŀռ��С
	if (buffer_mapped(map_bh) && block_in_file > *first_logical_block &&
			block_in_file < (*first_logical_block + nblocks)) {
		//����ʱfirst_logical_blockʼ����0����Ϊ64*4k�ļ����������ֲ���������̿飬��map_offset=block_in_file
		unsigned map_offset = block_in_file - *first_logical_block;
		unsigned last = nblocks - map_offset;

        //page_block��ֵ��0�����forѭ���ǰ�һ��pageҳ��Ӧ�����д��������ĵ�ַ��¼��blocks[page_block]�����ext4�ļ�ϵͳһ��
        //���������1k����blocks_per_page=4��ѭ��4�Σ����ext4�ļ�ϵͳһ�����������4K����blocks_per_page=1��ѭ��1��
		for (relative_block = 0; ; relative_block++) {
			if (relative_block == last) {
				clear_buffer_mapped(map_bh);
				break;
			}

            //blocks_per_page��1����1��forѭ��page_block��0����2��forѭ��page_block��1��if����break
			if (page_block == blocks_per_page)
				break;
            
            //����ʱ�ļ�64*4k,blocks[page_block]���α����ļ�0~64*4k����ӳ��Ĵ��̴��������ţ�һ��ext4���������4K��С
			blocks[page_block] = map_bh->b_blocknr + map_offset +
						relative_block;
			page_block++;
			block_in_file++;
		}
		bdev = map_bh->b_bdev;
	}


	/*
	 * Then do more get_blocks calls until we are done with this page.
	 */
	map_bh->b_page = page;
    //page_block��ʾһ��page��Ӧ�ĵڼ�����������飬��ֵ��0��ÿ��ѭ����1
	while (page_block < blocks_per_page) {//����ʱֻ�е�һ�γ���
		map_bh->b_state = 0;
		map_bh->b_size = 0;

		if (block_in_file < last_block) {
            //���ֶ�ȡ���ļ���С������ʱ��64*4k
			map_bh->b_size = (last_block-block_in_file) << blkbits;
            
        /*���ݱ��ζ�д���ļ���ʼ�����߼����block_in_file������ȡ���ļ�������map_bh->b_size������ļ������߼���ַ��һƬ�����Ĵ���������
            ӳ�䡣�������غ�bh->b_blocknr��ӳ��Ĵ����������ʼ��ַ��map_bh->b_size��ʵ�����ӳ��������������������*���С����������bh->b_state
            ��"mapped"״̬��buffer_mapped(map_bh)�������
            
       ע�⣬���ζ�ȡ�ļ������߼���ַ������һ��ȫ�����ӳ�䡣�����ļ�64*4K��С��������64*4K������������̷ֳ����飬�����
       �����ַ0~32*4k��64*4k~96*4k�����һ��ִ��ext4_get_block����ļ���ַ0~32*4K����������¼��0~32*4k��ӳ�䣬
       ֮���֪�����ļ��߼���ַ0~32*4Kӳ��Ĵ��������¼���ַ����ִ��submit_bio()����Щ���������¼���ȡ
       �ļ�ǰ4k*32�����ݵ��ļ��߼���ַ0~4k*32ӳ��page�ļ�ҳ������ִ��ext4_get_block����ļ��߼���ַ32*4~64*4K��
       ���������¼��64*4k~96*4k��ӳ�䣬֮���֪�����ļ��߼���ַ32*4~64*4Kӳ���
       ���������¼���ַ����ִ��submit_bio()����Щ���������¼���ȡ�ļ���4k*32�����ݵ��ļ��߼���ַ4k*32~4k*64ӳ��page�ļ�ҳ��
       ��Ȼ������ļ�64*4K�����ڴ��������¼���������ֲ�����ִ��һ��ext4_get_block()���ܵõ��ļ�0~64*4k�������ڴ��������¼��ĵ�ַ��
       ��ִ��submit_bio()����Щ���������¼���ȡ�ļ�4k*64�����ݵ��ļ��߼���ַ0~4k*64ӳ��page�ļ�ҳ��
       */
			if (get_block(inode, block_in_file, map_bh, 0))//ext4_get_block���ɹ�����0
				goto confused;
            
			*first_logical_block = block_in_file;
		}

		if (!buffer_mapped(map_bh)) {//����ʱ������
			fully_mapped = 0;
			if (first_hole == blocks_per_page)
				first_hole = page_block;
			page_block++;
			block_in_file++;
			continue;
		}

		/* some filesystems will copy data into the page during
		 * the get_block call, in which case we don't want to
		 * read it again.  map_buffer_to_page copies the data
		 * we just collected from get_block into the page's buffers
		 * so readpage doesn't have to repeat the get_block call
		 */
		if (buffer_uptodate(map_bh)) {//����ʱ������
			map_buffer_to_page(page, map_bh, page_block);
			goto confused;
		}
	
		if (first_hole != blocks_per_page)//����ʱ������
			goto confused;		/* hole -> non-hole */

		/* Contiguous blocks? */
		if (page_block && blocks[page_block-1] != map_bh->b_blocknr-1)//����ʱ������
			goto confused;
        
		nblocks = map_bh->b_size >> blkbits;//����ʱ�ļ�64*4k��map_bh->b_size=64*4k��nblocks=64

        //page_block��ֵ��0�����forѭ���ǰ�һ��pageҳ��Ӧ�����д��������ĵ�ַ��¼��blocks[page_block]�����ext4�ļ�ϵͳһ��
        //���������1k����blocks_per_page=4��ѭ��4�Σ����ext4�ļ�ϵͳһ�����������4K����blocks_per_page=1��ѭ��1��
		for (relative_block = 0; ; relative_block++) {
			if (relative_block == nblocks) {
				clear_buffer_mapped(map_bh);
				break;
            //blocks_per_page��1����1��forѭ��page_block��0����2��forѭ��page_block��1��if����break
			} else if (page_block == blocks_per_page)
				break;
            
            //����ʱ�ļ�64*4k,blocks[page_block]���α����ļ�0~64*4k����ӳ��Ĵ��̴��������ţ�һ��ext4���������4K��С
			blocks[page_block] = map_bh->b_blocknr + relative_block;
			page_block++;
			block_in_file++;
		}
		bdev = map_bh->b_bdev;
	}

	if (first_hole != blocks_per_page) {//����ʱ������
		zero_user_segment(page, first_hole << blkbits, PAGE_CACHE_SIZE);
		if (first_hole == 0) {
			SetPageUptodate(page);
			unlock_page(page);
			goto out;
		}
	} else if (fully_mapped) {//����ʱ����
		SetPageMappedToDisk(page);
	}

	if (fully_mapped && blocks_per_page == 1 && !PageUptodate(page) &&
	    cleancache_get_page(page) == 0) {//����ʱ������
		SetPageUptodate(page);
		goto confused;
	}

	/*
	 * This page will go to BIO.  Do we need to send this BIO off first?
	 */
	 
	/*���if�������������Ĵ��������ʱ������*last_block_in_bio����һ��page�ļ�ҳӳ��Ĵ���������ַ��blocks[0]����pageӳ��Ĵ���
    ������ַ�����������ַ��������if������������Ҫִ��mpage_bio_submit��bio���ļ����ݷ��͵Ĵ�������顣�����ž�Ҫִ��mpage_alloc
    �ٷ���һ��bio���������ǿ����ˣ�һ��bio�������ļ������߼���ַӳ���һƬ������������飬һ�������������Ĵ�������顣��Ҫ��ִ��
    ext4_get_block()��� �ļ������߼���ַ����һƬ��������������ӳ�䣬�����blocks[0]������ӳ���һƬ�������������ĵ�һ�������
    �ĵ�ַ��*/
	if (bio && (*last_block_in_bio != blocks[0] - 1))//
		bio = mpage_bio_submit(READ, bio);//���ִ��submit_bio

alloc_new:
	if (bio == NULL) {
        //����bio��bio->bi_sector�򱣴�64*4k��С�ļ��ĵ�һ�����������ţ���512��СΪ��λ
		bio = mpage_alloc(bdev, blocks[0] << (blkbits - 9),//blocks[0]<<(blkbits - 9)�ǰѴ��������ű����512Ϊ��λ����
			  	min_t(int, nr_pages, bio_get_nr_vecs(bdev)),
				GFP_KERNEL);
		if (bio == NULL)
			goto confused;
	}

    //����ʱ��first_hole ʼ����1��first_hole << blkbits��4K
	length = first_hole << blkbits;
    //Ϊ����Ҫ������ļ�����page����һ��bio_vec�ṹ����¼�ļ�ҳpage�ڴ��ַ���ļ�ҳ���ݴ�С��Ȼ���bio_vec��ӵ�bio
	if (bio_add_page(bio, page, length, 0) < length) {
		bio = mpage_bio_submit(READ, bio);//���ִ��submit_bio
		goto alloc_new;
	}

	relative_block = block_in_file - *first_logical_block;
    //����ʱ���ļ�64*4k��С�����������ֲ�������̿飬nblocks��64
	nblocks = map_bh->b_size >> blkbits;
	if ((buffer_boundary(map_bh) && relative_block == nblocks) ||
	    (first_hole != blocks_per_page))//����ʱ������
		bio = mpage_bio_submit(READ, bio);
	else//last_block_in_bio��¼�����ļ�ҳpageӳ������һ������������ַ�������ļ�����һ������������ַ��
	    //������������ַ1K��С��page�ļ�ҳӳ����4������������ַ��last_block_in_bio��¼��4������������ַ�����һ��
		*last_block_in_bio = blocks[blocks_per_page - 1];
out:
	return bio;

confused:
	if (bio)
		bio = mpage_bio_submit(READ, bio);
	if (!PageUptodate(page))
	        block_read_full_page(page, get_block);
	else
		unlock_page(page);
	goto out;
}

/**
 * mpage_readpages - populate an address space with some pages & start reads against them
 * @mapping: the address_space
 * @pages: The address of a list_head which contains the target pages.  These
 *   pages have their ->index populated and are otherwise uninitialised.
 *   The page at @pages->prev has the lowest file offset, and reads should be
 *   issued in @pages->prev to @pages->next order.
 * @nr_pages: The number of pages at *@pages
 * @get_block: The filesystem's block mapper function.
 *
 * This function walks the pages and the blocks within each page, building and
 * emitting large BIOs.
 *
 * If anything unusual happens, such as:
 *
 * - encountering a page which has buffers
 * - encountering a page which has a non-hole after a hole
 * - encountering a page with non-contiguous blocks
 *
 * then this code just gives up and calls the buffer_head-based read function.
 * It does handle a page which has holes at the end - that is a common case:
 * the end-of-file on blocksize < PAGE_CACHE_SIZE setups.
 *
 * BH_Boundary explanation:
 *
 * There is a problem.  The mpage read code assembles several pages, gets all
 * their disk mappings, and then submits them all.  That's fine, but obtaining
 * the disk mappings may require I/O.  Reads of indirect blocks, for example.
 *
 * So an mpage read of the first 16 blocks of an ext2 file will cause I/O to be
 * submitted in the following order:
 * 	12 0 1 2 3 4 5 6 7 8 9 10 11 13 14 15 16
 *
 * because the indirect block has to be read to get the mappings of blocks
 * 13,14,15,16.  Obviously, this impacts performance.
 *
 * So what we do it to allow the filesystem's get_block() function to set
 * BH_Boundary when it maps block 11.  BH_Boundary says: mapping of the block
 * after this one will require I/O against a block which is probably close to
 * this one.  So you should push what I/O you have currently accumulated.
 *
 * This all causes the disk requests to be issued in the correct order.
 */
//��ȡ���ļ�ҳpage������struct list_head *pages�������nr_pages�Ƕ�ȡpage����
//get_block��ext4_get_block��������ļ��߼���ַת�ɸ��ļ�ҳ����ʵ�ʱ����ڿ��豸��������ַ
int
mpage_readpages(struct address_space *mapping, struct list_head *pages,
				unsigned nr_pages, get_block_t get_block)//get_block:ext4_get_block
{
	struct bio *bio = NULL;
	unsigned page_idx;
	sector_t last_block_in_bio = 0;
	struct buffer_head map_bh;
	unsigned long first_logical_block = 0;

	map_bh.b_state = 0;
	map_bh.b_size = 0;
    //����ȡ��nr_pages��page��ִ��submit_bio�����page�ļ�ҳ��Ӧ�Ĵ������ݴӶ�Ӧ����������ȡ��page�ļ�ҳ�ڴ�
	for (page_idx = 0; page_idx < nr_pages; page_idx++) {
		struct page *page = list_entry(pages->prev, struct page, lru);

		prefetchw(&page->flags);
		list_del(&page->lru);
        
        //page��������index��ӵ�radix tree�����Ұ�page��ӵ�LRU_INACTIVE_FILE����
		if (!add_to_page_cache_lru(page, mapping,
					page->index, GFP_KERNEL)) {
			//��һ�θ�ѭ���������bio��NULL��֮���ѭ��bio����NULL
			bio = do_mpage_readpage(bio, page,//���ִ��submit_bio
					nr_pages - page_idx,
					&last_block_in_bio, &map_bh,
					&first_logical_block,
					get_block);
		}
		page_cache_release(page);
	}
	BUG_ON(!list_empty(pages));
	if (bio)
		mpage_bio_submit(READ, bio);//���ִ��submit_bio
	return 0;
}
EXPORT_SYMBOL(mpage_readpages);

/*
 * This isn't called much at all
 */
int mpage_readpage(struct page *page, get_block_t get_block)
{
	struct bio *bio = NULL;
	sector_t last_block_in_bio = 0;
	struct buffer_head map_bh;
	unsigned long first_logical_block = 0;

	map_bh.b_state = 0;
	map_bh.b_size = 0;
	bio = do_mpage_readpage(bio, page, 1, &last_block_in_bio,
			&map_bh, &first_logical_block, get_block);
	if (bio)
		mpage_bio_submit(READ, bio);
	return 0;
}
EXPORT_SYMBOL(mpage_readpage);

/*
 * Writing is not so simple.
 *
 * If the page has buffers then they will be used for obtaining the disk
 * mapping.  We only support pages which are fully mapped-and-dirty, with a
 * special case for pages which are unmapped at the end: end-of-file.
 *
 * If the page has no buffers (preferred) then the page is mapped here.
 *
 * If all blocks are found to be contiguous then the page can go into the
 * BIO.  Otherwise fall back to the mapping's writepage().
 * 
 * FIXME: This code wants an estimate of how many pages are still to be
 * written, so it can intelligently allocate a suitably-sized BIO.  For now,
 * just allocate full-size (16-page) BIOs.
 */

struct mpage_data {
	struct bio *bio;
	sector_t last_block_in_bio;
	get_block_t *get_block;
	unsigned use_writepage;
};

static int __mpage_writepage(struct page *page, struct writeback_control *wbc,
		      void *data)
{
	struct mpage_data *mpd = data;
	struct bio *bio = mpd->bio;
	struct address_space *mapping = page->mapping;
	struct inode *inode = page->mapping->host;
	const unsigned blkbits = inode->i_blkbits;
	unsigned long end_index;
	const unsigned blocks_per_page = PAGE_CACHE_SIZE >> blkbits;
	sector_t last_block;
	sector_t block_in_file;
	sector_t blocks[MAX_BUF_PER_PAGE];
	unsigned page_block;
	unsigned first_unmapped = blocks_per_page;
	struct block_device *bdev = NULL;
	int boundary = 0;
	sector_t boundary_block = 0;
	struct block_device *boundary_bdev = NULL;
	int length;
	struct buffer_head map_bh;
	loff_t i_size = i_size_read(inode);
	int ret = 0;

	if (page_has_buffers(page)) {
		struct buffer_head *head = page_buffers(page);
		struct buffer_head *bh = head;

		/* If they're all mapped and dirty, do it */
		page_block = 0;
		do {
			BUG_ON(buffer_locked(bh));
			if (!buffer_mapped(bh)) {
				/*
				 * unmapped dirty buffers are created by
				 * __set_page_dirty_buffers -> mmapped data
				 */
				if (buffer_dirty(bh))
					goto confused;
				if (first_unmapped == blocks_per_page)
					first_unmapped = page_block;
				continue;
			}

			if (first_unmapped != blocks_per_page)
				goto confused;	/* hole -> non-hole */

			if (!buffer_dirty(bh) || !buffer_uptodate(bh))
				goto confused;
			if (page_block) {
				if (bh->b_blocknr != blocks[page_block-1] + 1)
					goto confused;
			}
			blocks[page_block++] = bh->b_blocknr;
			boundary = buffer_boundary(bh);
			if (boundary) {
				boundary_block = bh->b_blocknr;
				boundary_bdev = bh->b_bdev;
			}
			bdev = bh->b_bdev;
		} while ((bh = bh->b_this_page) != head);

		if (first_unmapped)
			goto page_is_mapped;

		/*
		 * Page has buffers, but they are all unmapped. The page was
		 * created by pagein or read over a hole which was handled by
		 * block_read_full_page().  If this address_space is also
		 * using mpage_readpages then this can rarely happen.
		 */
		goto confused;
	}

	/*
	 * The page has no buffers: map it to disk
	 */
	BUG_ON(!PageUptodate(page));
	block_in_file = (sector_t)page->index << (PAGE_CACHE_SHIFT - blkbits);
	last_block = (i_size - 1) >> blkbits;
	map_bh.b_page = page;
	for (page_block = 0; page_block < blocks_per_page; ) {

		map_bh.b_state = 0;
		map_bh.b_size = 1 << blkbits;
		if (mpd->get_block(inode, block_in_file, &map_bh, 1))
			goto confused;
		if (buffer_new(&map_bh))
			unmap_underlying_metadata(map_bh.b_bdev,
						map_bh.b_blocknr);
		if (buffer_boundary(&map_bh)) {
			boundary_block = map_bh.b_blocknr;
			boundary_bdev = map_bh.b_bdev;
		}
		if (page_block) {
			if (map_bh.b_blocknr != blocks[page_block-1] + 1)
				goto confused;
		}
		blocks[page_block++] = map_bh.b_blocknr;
		boundary = buffer_boundary(&map_bh);
		bdev = map_bh.b_bdev;
		if (block_in_file == last_block)
			break;
		block_in_file++;
	}
	BUG_ON(page_block == 0);

	first_unmapped = page_block;

page_is_mapped:
	end_index = i_size >> PAGE_CACHE_SHIFT;
	if (page->index >= end_index) {
		/*
		 * The page straddles i_size.  It must be zeroed out on each
		 * and every writepage invocation because it may be mmapped.
		 * "A file is mapped in multiples of the page size.  For a file
		 * that is not a multiple of the page size, the remaining memory
		 * is zeroed when mapped, and writes to that region are not
		 * written out to the file."
		 */
		unsigned offset = i_size & (PAGE_CACHE_SIZE - 1);

		if (page->index > end_index || !offset)
			goto confused;
		zero_user_segment(page, offset, PAGE_CACHE_SIZE);
	}

	/*
	 * This page will go to BIO.  Do we need to send this BIO off first?
	 */
	if (bio && mpd->last_block_in_bio != blocks[0] - 1)
		bio = mpage_bio_submit(WRITE, bio);

alloc_new:
	if (bio == NULL) {
		bio = mpage_alloc(bdev, blocks[0] << (blkbits - 9),
				bio_get_nr_vecs(bdev), GFP_NOFS|__GFP_HIGH);
		if (bio == NULL)
			goto confused;
	}

	/*
	 * Must try to add the page before marking the buffer clean or
	 * the confused fail path above (OOM) will be very confused when
	 * it finds all bh marked clean (i.e. it will not write anything)
	 */
	length = first_unmapped << blkbits;
	if (bio_add_page(bio, page, length, 0) < length) {
		bio = mpage_bio_submit(WRITE, bio);
		goto alloc_new;
	}

	/*
	 * OK, we have our BIO, so we can now mark the buffers clean.  Make
	 * sure to only clean buffers which we know we'll be writing.
	 */
	if (page_has_buffers(page)) {
		struct buffer_head *head = page_buffers(page);
		struct buffer_head *bh = head;
		unsigned buffer_counter = 0;

		do {
			if (buffer_counter++ == first_unmapped)
				break;
			clear_buffer_dirty(bh);
			bh = bh->b_this_page;
		} while (bh != head);

		/*
		 * we cannot drop the bh if the page is not uptodate
		 * or a concurrent readpage would fail to serialize with the bh
		 * and it would read from disk before we reach the platter.
		 */
		if (buffer_heads_over_limit && PageUptodate(page))
			try_to_free_buffers(page);
	}

	BUG_ON(PageWriteback(page));
	set_page_writeback(page);
	unlock_page(page);
	if (boundary || (first_unmapped != blocks_per_page)) {
		bio = mpage_bio_submit(WRITE, bio);
		if (boundary_block) {
			write_boundary_block(boundary_bdev,
					boundary_block, 1 << blkbits);
		}
	} else {
		mpd->last_block_in_bio = blocks[blocks_per_page - 1];
	}
	goto out;

confused:
	if (bio)
		bio = mpage_bio_submit(WRITE, bio);

	if (mpd->use_writepage) {
		ret = mapping->a_ops->writepage(page, wbc);
	} else {
		ret = -EAGAIN;
		goto out;
	}
	/*
	 * The caller has a ref on the inode, so *mapping is stable
	 */
	mapping_set_error(mapping, ret);
out:
	mpd->bio = bio;
	return ret;
}

/**
 * mpage_writepages - walk the list of dirty pages of the given address space & writepage() all of them
 * @mapping: address space structure to write
 * @wbc: subtract the number of written pages from *@wbc->nr_to_write
 * @get_block: the filesystem's block mapper function.
 *             If this is NULL then use a_ops->writepage.  Otherwise, go
 *             direct-to-BIO.
 *
 * This is a library function, which implements the writepages()
 * address_space_operation.
 *
 * If a page is already under I/O, generic_writepages() skips it, even
 * if it's dirty.  This is desirable behaviour for memory-cleaning writeback,
 * but it is INCORRECT for data-integrity system calls such as fsync().  fsync()
 * and msync() need to guarantee that all the data which was dirty at the time
 * the call was made get new I/O started against them.  If wbc->sync_mode is
 * WB_SYNC_ALL then we were called for data integrity and we must wait for
 * existing IO to complete.
 */
int
mpage_writepages(struct address_space *mapping,
		struct writeback_control *wbc, get_block_t get_block)
{
	struct blk_plug plug;
	int ret;

	blk_start_plug(&plug);

	if (!get_block)
		ret = generic_writepages(mapping, wbc);
	else {
		struct mpage_data mpd = {
			.bio = NULL,
			.last_block_in_bio = 0,
			.get_block = get_block,
			.use_writepage = 1,
		};

		ret = write_cache_pages(mapping, wbc, __mpage_writepage, &mpd);
		if (mpd.bio)
			mpage_bio_submit(WRITE, mpd.bio);
	}
	blk_finish_plug(&plug);
	return ret;
}
EXPORT_SYMBOL(mpage_writepages);

int mpage_writepage(struct page *page, get_block_t get_block,
	struct writeback_control *wbc)
{
	struct mpage_data mpd = {
		.bio = NULL,
		.last_block_in_bio = 0,
		.get_block = get_block,
		.use_writepage = 0,
	};
	int ret = __mpage_writepage(page, wbc, &mpd);
	if (mpd.bio)
		mpage_bio_submit(WRITE, mpd.bio);
	return ret;
}
EXPORT_SYMBOL(mpage_writepage);
