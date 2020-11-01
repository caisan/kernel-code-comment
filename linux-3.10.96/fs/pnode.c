/*
 *  linux/fs/pnode.c
 *
 * (C) Copyright IBM Corporation 2005.
 *	Released under GPL v2.
 *	Author : Ram Pai (linuxram@us.ibm.com)
 *
 */
#include <linux/mnt_namespace.h>
#include <linux/mount.h>
#include <linux/fs.h>
#include <linux/nsproxy.h>
#include "internal.h"
#include "pnode.h"

/* return the next shared peer mount of @p */
static inline struct mount *next_peer(struct mount *p)//����peer groupͬһ��mount���е���һ��mount
{
	return list_entry(p->mnt_share.next, struct mount, mnt_share);
}

static inline struct mount *first_slave(struct mount *p)
{//���Ǵ�mnt_slave_list����ȡ����slave������ͬһ��slave mount���mount
	return list_entry(p->mnt_slave_list.next, struct mount, mnt_slave);
}

static inline struct mount *next_slave(struct mount *p)//����ͬ��slave����mount�����һ��mount
{
	return list_entry(p->mnt_slave.next, struct mount, mnt_slave);
}

static struct mount *get_peer_under_root(struct mount *mnt,
					 struct mnt_namespace *ns,
					 const struct path *root)
{
	struct mount *m = mnt;

	do {
		/* Check the namespace first for optimization */
		if (m->mnt_ns == ns && is_path_reachable(m, m->mnt.mnt_root, root))
			return m;

		m = next_peer(m);
	} while (m != mnt);

	return NULL;
}

/*
 * Get ID of closest dominating peer group having a representative
 * under the given root.
 *
 * Caller must hold namespace_sem
 */
int get_dominating_id(struct mount *mnt, const struct path *root)
{
	struct mount *m;

	for (m = mnt->mnt_master; m != NULL; m = m->mnt_master) {
		struct mount *d = get_peer_under_root(m, mnt->mnt_ns, root);
		if (d)
			return d->mnt_group_id;
	}

	return 0;
}

static int do_make_slave(struct mount *mnt)
{
	struct mount *peer_mnt = mnt, *master = mnt->mnt_master;
	struct mount *slave_mnt;

	/*
	 * slave 'mnt' to a peer mount that has the
	 * same root dentry. If none is available then
	 * slave it to anything that is available.
	 */
	while ((peer_mnt = next_peer(peer_mnt)) != mnt &&
	       peer_mnt->mnt.mnt_root != mnt->mnt.mnt_root) ;

	if (peer_mnt == mnt) {
		peer_mnt = next_peer(mnt);
		if (peer_mnt == mnt)
			peer_mnt = NULL;
	}
	if (mnt->mnt_group_id && IS_MNT_SHARED(mnt) &&
	    list_empty(&mnt->mnt_share))
		mnt_release_group_id(mnt);

	list_del_init(&mnt->mnt_share);
	mnt->mnt_group_id = 0;

	if (peer_mnt)
		master = peer_mnt;

	if (master) {
		list_for_each_entry(slave_mnt, &mnt->mnt_slave_list, mnt_slave)
			slave_mnt->mnt_master = master;
		list_move(&mnt->mnt_slave, &master->mnt_slave_list);
		list_splice(&mnt->mnt_slave_list, master->mnt_slave_list.prev);
		INIT_LIST_HEAD(&mnt->mnt_slave_list);
	} else {
		struct list_head *p = &mnt->mnt_slave_list;
		while (!list_empty(p)) {
                        slave_mnt = list_first_entry(p,
					struct mount, mnt_slave);
			list_del_init(&slave_mnt->mnt_slave);
			slave_mnt->mnt_master = NULL;
		}
	}
	mnt->mnt_master = master;
	CLEAR_MNT_SHARED(mnt);
	return 0;
}

/*
 * vfsmount lock must be held for write
 */
void change_mnt_propagation(struct mount *mnt, int type)
{
	if (type == MS_SHARED) {
		set_mnt_shared(mnt);
		return;
	}
	do_make_slave(mnt);
	if (type != MS_SLAVE) {
		list_del_init(&mnt->mnt_slave);
		mnt->mnt_master = NULL;
		if (type == MS_UNBINDABLE)
			mnt->mnt.mnt_flags |= MNT_UNBINDABLE;
		else
			mnt->mnt.mnt_flags &= ~MNT_UNBINDABLE;
	}
}

/*
 * get the next mount in the propagation tree.
 * @m: the mount seen last
 * @origin: the original mount from where the tree walk initiated
 *
 * Note that peer groups form contiguous segments of slave lists.
 * We rely on that in get_source() to be able to find out if
 * vfsmount found while iterating with propagation_next() is
 * a peer of one we'd found earlier.
 */
/*
  mount1_1��dest mount����������share mount�顣mount1_1��mount1_2��mount_1_3����һ��slave mount�飬���ǵ�mnt_master��ָ��mount1,
  �±�û�л�ȫ,����slave mount���mount->mnt_master��ָ�����¡ĸ�塣
  
  ���豾�β���ʱ mount --bind /home/test  /home/
  
  mount1---mount2---mount3---mount4---mount5 (share/peer mount��1)
     |       |
     |       |
     |       mount2_1---mount2_2---mount2_3---mount2_4---mount2_5  (slave mount��1)
     |                                                      
    mount1_1---mount1_2---mount_1_3  (slave mount��2)      
                  |
                  mount1_2_1---mount1_2_2---mount1_2_3 (slave mount��3)
                  
   �±���mount1���dest mountΪ�����ǳ���ϸ���ܴ������̣����˸�����һĿ��Ȼ��û����ǿ���Գ�ǿ�Ŀռ�����ȴ��������ֻ�뿿�������⣬����
   
step 1:mount1��slave mount��¡ĸ�壬��һ��ִ��propagation_next()��ֱ��return first_slave(m)������slave mount��mount_1_1

step 2:m��mount_1_1��m->mnt_master��mount_1��origin->mnt_master��mount1->mnt_master��NULL��if (master == origin->mnt_master)��������
  else if (m->mnt_slave.next != &master->mnt_slave_list)����������жϵ���˼��m�Ƿ����slave mount������һ��mount����mount_1_3��
  master->mnt_slave_list��mount_1->mnt_slave_list�����ϵı������slave mount��ĵ�һ��mount�����m��slave mount������һ��mount��
  ��m->mnt_slave.nextҲ��slave mount��ĵ�һ��mount�����m->mnt_slave.next == &master->mnt_slave_list��˵�����slave mount�������mount
  ���������ˡ�m��mount_1_1ʱ��ִ��return next_slave(m)����mount_1_2��
  
step 3:�´�ִ��propagation_next()��m��mount_1_2��mount_1_2��slave mount��¡ĸ�壬��ִ��return first_slave(m)��������slave mount��
  ��mount1_2_1��
  
step 4:�´�ִ��propagation_next()��m��mount1_2_1,m->mnt_master��mount1_2��if (master == origin->mnt_master)�϶����������±ߵ�
  else if (m->mnt_slave.next != &master->mnt_slave_list)������ִ��return next_slave(m)����mount1_2_2���´�ִ��propagation_next()��
  ִ��return next_slave(m)����mount1_2_3���´�ִ��propagation_next()��m��mount1_2_3��mount1_2_3��slave mount��3�����һ��mount��
  m->mnt_slave.next��&master->mnt_slave_list��slave mount��3�ĵ�һ��mount���Ǹ�else if����������mount��������ˣ���ִ��m = master��
  m��Ϊslave mount��3��ĸ��mount_1_2����ʱ�൱�ڷ�����slave mount��2�����ű���slave mount��2��mount��ִ��return next_slave(m)
  ����mount_1_3
  
step 5:�´�ִ��propagation_next()��m��mount_1_3��mount_1_3��slave mount��2�����һ��mount��m->mnt_slave.next��&master->mnt_slave_list
  ��slave mount��2�ĵ�һ��mount���Ǹ�else if����������mount��������ˣ���ִ��m = master��m��Ϊslave mount��2��ĸ��mount_1��
  ��ʱ�൱�ڷ�����"share/peer mount��1"��m->mnt_master��mount_1->mnt_master��NULL��origin->mnt_masterҲ��mount_1->mnt_master��
  if (master == origin->mnt_master)��������ִ��next_peer(m)������mount2��
  
step 6:�´�ִ��propagation_next()��m��mount_2��mount_2��slave mount��1��ĸ�壬��ʱ��Ҫ����step2~step4��һ��������slave mount��1��
  mount2_1~mount2_5�����أ�������̲���˵�ˣ���step2~step4�����ٴ�propagation_next()��m��mount2_5����slave mount��1�����һ��mount��
  ֱ��ִ��m = master��m��Ϊslave mount��1ĸ��mount_2�����Żص�while(1)ѭ����ͷ��mount_2->mnt_master��NULL��
  if (master == origin->mnt_master)������ִ��next_peer(m)������mount_3��

step 7:�´�ִ��propagation_next()��m��mount_3��mount_3->mnt_master��NULL��if (master == origin->mnt_master)������ִ��next_peer(m)
  ������mount_4���´�ִ��propagation_next()��m��mount_4��mount_4->mnt_master��NULL��if (master == origin->mnt_master)������
  ִ��next_peer(m)������mount_5���´�ִ��propagation_next()��m��mount_5��mount_5->mnt_master��NULL��if (master == origin->mnt_master)
  ���������Ǻܿ�ϧ����ʱnext=next_peer(m)��������ͷmount1��(next == origin)������propagation_next()���ڷ���NULL����ˣ��Ա���dest mount
  ��mount1�Ĵ������������������propagation_next()����ţ��ѽ���̶�10�д��룬�Ͱ���ô�����mount��ϵ��������!!!!!

step 8:��û�п���dest mount��mount1_1��������dest mount�������һ��slave mount���mount��Ա?����Ϊû�п��ܣ���Ϊһ��ʵ�ʵ�mount��
  �Ǳ�ȻҪִ��mount /dev/sda3 /home/���ص�ĳ��Ŀ¼�γɵģ�һ�����ص�Ŀ¼����Ĺ���Դ�ļ�ϵͳmount��һ������ʵ���ļ�ϵͳ��Ӧ�ģ�����
  mount�봫���γɵ�slave mount����share mount����private mount��һ��������dest mount����Ĺ��ص�Ŀ¼�����ļ�ϵͳ��mount��Ӧ����Ĭ�ϵ�
  ���ԣ���share���ԣ��������ǿ�¡���ɵġ�mount --make-slave /dev/sdb  /mnt/test2/ ��ȻҲ���ԡ����ǿ��ǵ�һ�㶼��
  mount --bind ʱ�Ż�ָ�� --make-slave/--make-private/--make-share���ԡ����ھ���ʱ�϶���dest mount������privae���ԣ�������slave mount��ġ�

 �ܽᣬ���ڶ�mount�Ĵ�����������һ��������:��dest mountΪԴͷ����������shared mount����slave mount���mount�����������dest mount
 ����share mount�飬���Ա������share mount���һ������Ա�����ء����ǣ����share mount��mount������slave mount���ĸ�壬�����������
 �������slave mount�����slave mount�����أ��������򷵻�slave mountĸ�壬��������ĸ�����ڵ��Ǹ�mount�顣ע�⣬slave mount���mount
 Ҳ������slave mount���ĸ�壬���־ͼ���������һ���slave mount����mount��������ȫ����slave mount�ٷ���ĸ��mount��һ�㡣���û��
 slave mount���������̷ǳ��򵥣�һ��������share mount���mount�����ˡ������;�����и�mount��slave mount���ĸ�壬�Ǿͱ������
 slave mount���mount�������귵��ĸ����һ���mount����������
*/
//����peer group��ͬ��shared����mount���е���һ��mount����ͬ��slave����mount�����һ��mount��ò�����и���mount�ṹ��shared���Ե�mount
//������mnt_share��Ա����һ�������������и���mount�ṹ��slave���Ե�mount����mnt_slave��Ա����һ����������propagation_next()����
//ò�ƾ����Ա���mount�����dest mount�ṹΪ��ʼ��ͨ��mount�ṹ��mnt_share��mnt_slave��Ա����������ͬһ�����������е�mount�ṹ
static struct mount *propagation_next(struct mount *m,/*m��share mount�����slave mount�����mount����ֵ��dest mount*/
					 struct mount *origin)/*origin ��Զ�Ǳ��ε�dest mount*/
{
	// are there any slaves of this mount? 
    /*m������mount������m��slave mount��¡ĸ�壬�򷵻�������slave mount��m�ǿ�¡ĸ�壬m->mnt_slave_list�ҵľ�����slave mount����NULL��
      list_empty()����false��if������mount������slave����ʱ����¡soure mount ���ɵ���mount����mount��ӵ�ĸ��mount��mnt_slave_list����
      ���m->mnt_slave_listΪNULL����˵��m��û����slave mount����slave mount!*/
    if (!IS_MNT_NEW(m) && !list_empty(&m->mnt_slave_list))
        //���m����slave mount,��m->mnt_slave_list����ȡ������slave������ͬһ��slave mount���mount
		return first_slave(m);
    
    /*һ��mount�������slave����Ҳ��share���ԣ�share���Ե�mount�϶�����mnt_share��ӵ�ͬshare mount���mount��mnt_share����
    ��slave���Ե�mount����mnt_slave��ӵ�ͬslave mount���mount��mnt_slave���������ǿ�¡ĸ��mount��mnt_slave_list������������һ��
    mount������mount shared���mount������������ĳ��slave mount��mount�Ŀ�¡ĸ�塣���m������shared�������ϱߵ�ִ��first_slave(m)����
    ��slave mount��ĵ�һ��mount�����´�ѭ��ִ�иú������ͻ�ִ���±ߵ�mount��*/
	while (1) {
        //master��m�Ŀ�¡ĸ��mount�����mû��slave���ԣ�m->mnt_master��NULL��share��private���Ե�mount�ṹmnt_master����NULL
		struct mount *master = m->mnt_master;

        /*origin->mnt_master��Զ��dest mount��mnt_master�����dest mountû��private���ԣ�origin->mnt_master��Զ��NULL��master��ÿ�α���
         ��ͬһ��slave��shared mount���m��master��m�ĳ�ֵ����dest mount�����dest mount��һ��share mount���һ��mount���ǵ�һ���ж�
         origin->mnt_master��master����NULL��������return share mount���next mount���´�ִ�иú�����m�����Ǹ�next mount��m->mnt_master
         ����NULL��if (master == origin->mnt_master)�����������ŷ���shared mount���next next mount��ֱ�������꣬(next == origin)�򷵻�NULL��
         */
        //���m��share mount��ĳ�Ա����next_peer(m)����share mount�����һ��mount��ֱ��������share mount��ĵ�����mount������NULL
        //origin��dest mount������ʱ�϶�������slave mount��Ա��һ����Ĭ�ϵ�share���ԣ����ϱߵ�step 8������
		if (master == origin->mnt_master) {
			struct mount *next = next_peer(m);
			return (next == origin) ? NULL : next;
      //�����m��һ����slave mount��ĳ�Ա����next_slave(m)һֱ��������slave mount���mount��ֱ�������꣬if����������ִ���±ߵ�m = master;
		} else if (m->mnt_slave.next != &master->mnt_slave_list)
			return next_slave(m);

		// back at master 
		//ֻ��m��slave mount������һ��mount��mount���������ˣ��Ż�ִ�������m = master����m�����slave mount���ĸ��mount��
		//�൱�ڷ���ĸ��mount���ڵ���һ��mount������ѭ��������
		m = master;
	}
}

/*
 * return the source mount to be used for cloning
 *
 * @dest 	the current destination mount
 * @last_dest  	the last seen destination mount
 * @last_src  	the last seen source mount
 * @type	return CL_SLAVE if the new mount has to be
 * 		cloned as a slave.
 */
/*
propagate_mnt->propagation_next()�õ�slave mount�����share mount���һ��mount
             ->get_source()���õ�һ����¡ĸ���mount
             ->copy_tree()�������ϱߵĿ�¡ĸ��mount��¡һ��mount
���豾�β���ʱ mount --bind /home/test  /home/,mount1��dest mount

mount1---mount2---mount3---mount4---mount5 (share/peer mount��1)
   |       
   |                                                             
  mount1_1---mount1_2  (slave mount��2)      
                
1 mount1��dest mount����һ��ѭ����propagation_next()����mount_1_1��get_source(),dest����mount_1_1��last_dest�Ǳ���ԭʼdest mount��mount1��
  last_src�Ǳ���mount��ԭʼsource mount��while (last_dest != dest->mnt_master)��if (p_last_dest)������������*type = CL_SLAVE������
  ������¡��mountΪslave���ԣ�return last_src�����ر��ι��ص�ԭʼsource mount��Ȼ��copy_tree()����last_srcΪ��¡ĸ�壬
  ����mount�ṹchild_1_1�����mount��������slave��child_1_1->mnt_master��ԭʼsource mount����last_dest=mount_1_1��last_src=child_1_1��
  ��β�����propagate_mnt()�������ִ�У�last_dest����ֵΪ����propagation_next()���ص�m��last_src����ֵΪÿ�ο�¡���ɵ�child��
  
2 �ڶ���ѭ����propagation_next()����mount_1_2��get_source(),��dest����mount_1_2��last_dest��mount_1_1��last_src��child_1_1��
  while (last_dest != dest->mnt_master)��������last_dest = last_dest->mnt_master��last_dest���mount1,last_src = last_src->mnt_master
  ��last_src��ɱ��ι��ص�ԭʼsource mount�������˳�while��if (p_last_dest)��������û�ã�*type = CL_SLAVE��last_src��ʱ���Ǳ��ι���
  ��ԭʼsource mount����get_source()���Ƿ��ر��ι��ص�ԭʼsource mount��֮��ִ�е�copy_tree()��������ԭʼsource mount��¡mount��
  ��¡������child_1_2�����mount��������slave��child_1_2->mnt_master��ԭʼsource mount��
  ����ִ��propagate_mnt()��ߵĴ��룬last_dest=mount_1_2(��m)��last_src=child_1_2(��child)

3 ������ѭ����propagation_next()����mount2,����mount share���mount����ʱִ��get_source(),dest��mount2,last_src��child_1_2��
  last_dest��mount_1_2��dest->mnt_master��NULL��while��������p_last_dest = last_dest��p_last_dest���mount_1_2��p_last_src = last_src��
  p_last_src���child_1_2��last_dest = last_dest->mnt_master��last_dest���mount1��last_src = last_src->mnt_master��last_src���
  ԭʼsource mount��while��Ȼ����������ִ��while���4����ֵ��p_last_dest���mount1,p_last_src���ԭʼsource mount��last_dest���NULL��
  last_src���NULL��while��������if (p_last_dest)������p_last_dest = next_peer(p_last_dest)��p_last_dest���mount2��
  ��if (dest == p_last_dest)������*type = CL_MAKE_SHARED�����ص�mount��p_last_src��ԭʼsource mount��֮��ִ�е�copy_tree()��������
  ԭʼsource mount��¡mount����¡������child_2�����mount��������share������¡ĸ��source mount����ͬһ��share mount�顣
  ����ִ��propagate_mnt()��ߵĴ��룬last_dest=mount_2(��m)��last_src=child_2(��child)
  
4 ��4��ѭ����propagation_next()����mount3����ʱִ��get_source(),dest��mount3��last_src��child_2��last_dest=mount_2��while������
  ִ��while���4����ֵ��p_last_dest���mount_2��p_last_src���child_2��last_dest���NULL��last_src���NULL������share mount������
  mnt_master����NULL��if (p_last_dest)������p_last_dest = next_peer(p_last_dest)����mount_3,if (dest == p_last_dest)������
  ��*type = CL_MAKE_SHARED������p_last_src��last_src��child_2��Ȼ��ִ��copy_tree()����last_src��child_2��¡mount����¡������child_3��
  ���mount��������share������¡ĸ��child_2����ͬһ��share mount�飬���ԭʼsource mountҲ��һ��share mount�顣
  ����ִ��propagate_mnt()��ߵĴ��룬last_dest=mount_3(��m)��last_src=child_3(��child)
  
5 ��5��ѭ���͵�6��ѭ������������ѭ��ԭ���һ���ˣ�ÿ��get_source()ʱ��last_src������һ����¡���ɵ�child��Ȼ�󷵻����child����Ϊ��¡ĸ��
  ����¡�����µ�child:copy_tree()����child_3��¡����child_4(mount4��Ӧ��);copy_tree()��child_4��¡����child_5(mount5��Ӧ��)��

�ܽ�:get_source()�õ�dest ���mount��Ӧ��source mount�������ǣ������dest��slave mount����get_source()���ص���Զ�����һ����last_src
�Ǹ�mount���൱��destΪslave��get_source()���صĿ�¡ĸ����Զ��last_src�����last_src�������slave mount���¡ĸ���Ӧ���Ǹ�source mount
�ɡ��ǵģ�mount1��slave mount���Աmount1_1��mount1_2��destʱ��get_source���صľ��Ǳ��ι��ص�ԭʼsource mount�������ԭʼsouce mount��
mount1���Ǳ���mount bind��Դmount��Ŀ��mount�����dest ��share��mount��Ա��get_source()���ص�last_src����Զ����һ��ѭ����¡����child��
ʵʱ�ڱ䡣
1  get_source()dest��slave mount���Ա�����ص���Զ�����slave mount��Ŀ�¡ĸ���Ӧ��source mount��last_src�����source mount���䡣Ȼ��
   ÿһ��propagate_mnt()�ж���ִ��copy_tree()�������source mount��¡�����µ�mount��child��child->mnt_masterΪsource mount��
2  get_source()dest��share mount���Ա�����˵�һ���Ƿ��ر���mount���ص�source mount��Ȼ�����ſ�¡�����µ�mount��child��child�Ϳ�¡ĸ�� 
   ��ͬһ��mount�����顣Ȼ��last_src=child��֮��ļ���ѭ����get_source()���Ƿ����ϴο�¡����child��Ȼ���Դ�Ϊĸ���ٿ�¡�����п�¡����
   child���ͱ���mount���ص�source mount��һ��mount�����顣
*/
static struct mount *get_source(struct mount *dest,
				struct mount *last_dest,
				struct mount *last_src,
				int *type)
{
	struct mount *p_last_src = NULL;
	struct mount *p_last_dest = NULL;


	while (last_dest != dest->mnt_master) {
		p_last_dest = last_dest;
		p_last_src = last_src;
		last_dest = last_dest->mnt_master;
		last_src = last_src->mnt_master;
	}

	if (p_last_dest) {
		do {
			p_last_dest = next_peer(p_last_dest);
		} while (IS_MNT_NEW(p_last_dest));
		/* is that a peer of the earlier? */
		if (dest == p_last_dest) {
			*type = CL_MAKE_SHARED;
			return p_last_src;
		}
	}
	/* slave of the earlier, then */
	*type = CL_SLAVE;
	/* beginning of peer group among the slaves? */
	if (IS_MNT_SHARED(dest))
		*type |= CL_MAKE_SHARED;
	return last_src;
}

/*
 * mount 'source_mnt' under the destination 'dest_mnt' at
 * dentry 'dest_dentry'. And propagate that mount to
 * all the peer and slave mounts of 'dest_mnt'.
 * Link all the new mounts into a propagation tree headed at
 * source_mnt. Also link all the new mounts using ->mnt_list
 * headed at source_mnt's ->mnt_list
 *
 * @dest_mnt: destination mount.
 * @dest_dentry: destination dentry.
 * @source_mnt: source mount.
 * @tree_list : list of heads of trees to be attached.
 */
int propagate_mnt(struct mount *dest_mnt, struct mountpoint *dest_mp,
		    struct mount *source_mnt, struct list_head *tree_list)
{
	struct user_namespace *user_ns = current->nsproxy->mnt_ns->user_ns;
	struct mount *m, *child;
	int ret = 0;
	struct mount *prev_dest_mnt = dest_mnt;
	struct mount *prev_src_mnt  = source_mnt;
	LIST_HEAD(tmp_list);

//����peer group��ͬ��shared����mount���е���һ��mount����ͬ��slave����mount�����һ��mount��ò�����и���mount�ṹ��shared���Ե�mount
//������mnt_share��Ա����һ�������������и���mount�ṹ��slave���Ե�mount����mnt_slave��Ա����һ����������propagation_next()����
//ò�ƾ����Ա���mount�����dest mount�ṹΪ��ʼ��ͨ��mount�ṹ��mnt_share��mnt_slave��Ա����������ͬһ�����������е�mount�ṹ����m����
	for (m = propagation_next(dest_mnt, dest_mnt); m;
			m = propagation_next(m, dest_mnt)) {
		int type;
		struct mount *source;
        //����Ǳ��ι��������ɵ�mount�����ô�����mount bind������֮ǰ�Ѿ���¡������source mount����ԭĿ¼��mount��ͬһ��share �飬����������
		if (IS_MNT_NEW(m))
			continue;

		source =  get_source(m, prev_dest_mnt, prev_src_mnt, &type);

		/* Notice when we are propagating across user namespaces */
		if (m->mnt_ns->user_ns != user_ns)
			type |= CL_UNPRIVILEGED;

     /*ִ��clone_mnt()����source��¡һ��mount�����ÿ�¡��mount��mnt_mountpointΪ��¡ĸ���mnt_mountpoint����󷵻ؿ�¡mount��child*/
		child = copy_tree(source, source->mnt.mnt_root, type);
		if (IS_ERR(child)) {
			ret = PTR_ERR(child);
			list_splice(tree_list, tmp_list.prev);
			goto out;
		}

        /*dest_mp->m_dentry�Ǳ���mount���ص��ռ�Ŀ¼dentry��m->mnt.mnt_root��m���mount�ṹ����Ŀ��豸�ļ�ϵͳ�ĸ�Ŀ¼
          ֻ��dest_mp->m_dentry��m������ļ�ϵͳ�µ�Ŀ¼dentry����Ч*/
		if (is_subdir(dest_mp->m_dentry, m->mnt.mnt_root)) {
          /*��Կ�¡����mount�ṹchild���ø��ӹ�ϵ����¡���ɵ�child��"source mount"��m�ǹ��ص�Ŀ¼�Ŀ��豸��mount�ṹ������"dest mount"
            m��child��parent��dest_mp->m_dentry��child���ص�Ŀ¼dentry����dest_mp->m_dentry�϶�Ҳ����"dest mount"��m���mount�ṹ����
            �Ŀ��豸�ļ�ϵͳ�ĸ�Ŀ¼�µ�һ��Ŀ¼�����û������������if (is_subdir(dest_mp->m_dentry, m->mnt.mnt_root))����Ҫ�С�
             */
			mnt_set_mountpoint(m, dest_mp, child);
            /*��¡����child��ʱ��ӵ�tree_list������attach_recursive_mnt��󣬻��tree_list�����ϵĿ�¡���ɵ�mountȡ������ִ��
             attach_recursive_mnt����mount�ṹ��ӵ�ϵͳ*/
			list_add_tail(&child->mnt_hash, tree_list);
		} else {
			 // This can happen if the parent mount was bind mounted
			 // on some subdirectory of a shared/slave mount.
			/*��¡���ɵ�child��Ч���ȷŵ�tmp_list�����ú��������ִ��umount_tree()������Щmount*/
			list_add_tail(&child->mnt_hash, &tmp_list);
		}
        
        /*prev_dest_mntָ��propagation_next()����dest mount��slave mount�����share mount�鷵�ص�mount*/
		prev_dest_mnt = m;
        /*prev_src_mntָ���¡���ɵ�mount����child*/
		prev_src_mnt  = child;
	}
out:
	br_write_lock(&vfsmount_lock);
	while (!list_empty(&tmp_list)) {
		child = list_first_entry(&tmp_list, struct mount, mnt_hash);
		umount_tree(child, 0);
	}
	br_write_unlock(&vfsmount_lock);
	return ret;
}

/*
 * return true if the refcount is greater than count
 */
static inline int do_refcount_check(struct mount *mnt, int count)
{
	int mycount = mnt_get_count(mnt) - mnt->mnt_ghosts;
	return (mycount > count);
}

/*
 * check if the mount 'mnt' can be unmounted successfully.
 * @mnt: the mount to be checked for unmount
 * NOTE: unmounting 'mnt' would naturally propagate to all
 * other mounts its parent propagates to.
 * Check if any of these mounts that **do not have submounts**
 * have more references than 'refcnt'. If so return busy.
 *
 * vfsmount lock must be held for write
 */
int propagate_mount_busy(struct mount *mnt, int refcnt)
{
	struct mount *m, *child;
	struct mount *parent = mnt->mnt_parent;
	int ret = 0;

	if (mnt == parent)
		return do_refcount_check(mnt, refcnt);

	/*
	 * quickly check if the current mount can be unmounted.
	 * If not, we don't have to go checking for all other
	 * mounts
	 */
	if (!list_empty(&mnt->mnt_mounts) || do_refcount_check(mnt, refcnt))
		return 1;

	for (m = propagation_next(parent, parent); m;
	     		m = propagation_next(m, parent)) {
		child = __lookup_mnt(&m->mnt, mnt->mnt_mountpoint, 0);
		if (child && list_empty(&child->mnt_mounts) &&
		    (ret = do_refcount_check(child, 1)))
			break;
	}
	return ret;
}

/*
 * NOTE: unmounting 'mnt' naturally propagates to all other mounts its
 * parent propagates to.
 */
static void __propagate_umount(struct mount *mnt)
{
	struct mount *parent = mnt->mnt_parent;
	struct mount *m;

	BUG_ON(parent == mnt);

	for (m = propagation_next(parent, parent); m;
			m = propagation_next(m, parent)) {

		struct mount *child = __lookup_mnt(&m->mnt,
					mnt->mnt_mountpoint, 0);
		/*
		 * umount the child only if the child has no
		 * other children
		 */
		if (child && list_empty(&child->mnt_mounts))
			list_move_tail(&child->mnt_hash, &mnt->mnt_hash);
	}
}

/*
 * collect all mounts that receive propagation from the mount in @list,
 * and return these additional mounts in the same list.
 * @list: the list of mounts to be unmounted.
 *
 * vfsmount lock must be held for write
 */
int propagate_umount(struct list_head *list)
{
	struct mount *mnt;

	list_for_each_entry(mnt, list, mnt_hash)
		__propagate_umount(mnt);
	return 0;
}
