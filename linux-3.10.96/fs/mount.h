#include <linux/mount.h>
#include <linux/seq_file.h>
#include <linux/poll.h>

struct mnt_namespace {
	atomic_t		count;
	unsigned int		proc_inum;
	struct mount *	root;
    //ò������һ�������ռ���ļ�ϵͳ��struct mount�ṹ�嶼���������������ͬ�����ռ�˴˿�������Ӧ�����ռ���ļ�ϵͳmount�ṹ
	struct list_head	list;
	struct user_namespace	*user_ns;
	u64			seq;	/* Sequence number to prevent loops */
	wait_queue_head_t poll;
	int event;
};

struct mnt_pcp {
	int mnt_count;
	int mnt_writers;
};

//���豸�Ĺ��ص�Ŀ¼
struct mountpoint {
	struct list_head m_hash;
	struct dentry *m_dentry;//�ҵ��Ŀ¼dentry
	int m_count;
};

//ÿһ�����صĿ��豸��Ҫ����һ��mount�ṹ�壬ÿһ�ι��ض������ɵ�һ��mount�ṹ
struct mount {
    //mount��mnt_hash����mount hash����__lookup_mnt()�ǴӸ�mount hash��������mount�ṹ��commit_tree()��attach_mnt()�п�
    //mnt_hash��mount����mount hash������������hash��ļ�ֵ��(��mount�ṹ��vfsmount��Ա+��mount�Ĺ��ص�dentry)
	struct list_head mnt_hash;
    //��mount,attach_recursive_mnt->mnt_set_mountpoint(),��Ȼ����Ϊ�ҵ�Ŀ¼�����ļ�ϵͳ��mount��
    //Ҳ˵Ҳ�ǣ���mount��Ӧ����Ҫ���ڵ�Ŀ¼���ڵ��ļ�ϵͳ��mount�ṹ
	struct mount *mnt_parent;
    //���ص�dentry��attach_recursive_mnt->mnt_set_mountpoint()����Ϊ���ص�Ŀ¼dentry
	struct dentry *mnt_mountpoint;
    //�������豸�ĸ�Ŀ¼dentry
	struct vfsmount mnt;
#ifdef CONFIG_SMP
	struct mnt_pcp __percpu *mnt_pcp;
#else
	int mnt_count;
	int mnt_writers;
#endif
    //commit_tree()��mnt_child��mount�ṹ��ӵ�mount��parent mount��mnt_mounts�����������������mount����mount�ṹ���������
	struct list_head mnt_mounts;	/* list of children, anchored here */
    //next_mnt()�����mnt_child������mount�ṹ��commit_tree()��attach_mnt()��mnt_child��mount�ṹ��ӵ�mount��mnt_parent��mnt_mounts����
	struct list_head mnt_child;	/* and going through their mnt_child */
	struct list_head mnt_instance;	/* mount instance on sb->s_mounts */
   
	const char *mnt_devname;	/* Name of device e.g. /dev/dsk/hda1 */
    //copy_tree()��������mount����mnt_list��ӵ�������
	struct list_head mnt_list;
	struct list_head mnt_expire;	/* link in fs-specific expiry list */
    //clone_mnt()�ѱ��ι���shared���Եĵ�mount�ṹ���ӵ���һ��mount��mnt_share����
	struct list_head mnt_share;	/* circular list of shared mounts */
    //slave mounts
	struct list_head mnt_slave_list;/* list of slave mounts */
    //clone_mnt()�У��ѱ��ι���slave���Ե�mount�ṹ���ӵ���һ��mount��mnt_slave����
	struct list_head mnt_slave;	/* slave list entry */
    //clone_mnt()�У�����ֵΪold->mnt_master
	struct mount *mnt_master;	/* slave is on master->mnt_slave_list */
    //mount���������ռ䣬commit_tree()�а�mount�ṹ��ӵ���mount��mnt_ns��list����
	struct mnt_namespace *mnt_ns;	/* containing namespace */
    //���ص�ṹ���������ص�dentry��attach_recursive_mnt->mnt_set_mountpoint()������
	struct mountpoint *mnt_mp;	/* where is it mounted */
#ifdef CONFIG_FSNOTIFY
	struct hlist_head mnt_fsnotify_marks;
	__u32 mnt_fsnotify_mask;
#endif
    //mount id
	int mnt_id;			/* mount identifier */
    //mount group id��һ��mount������е�mount�ṹ��mnt_group_idһ��
	int mnt_group_id;		/* peer group identifier */
	int mnt_expiry_mark;		/* true if marked for expiry */
	int mnt_pinned;
	int mnt_ghosts;
};

#define MNT_NS_INTERNAL ERR_PTR(-EINVAL) /* distinct from any mnt_namespace */

static inline struct mount *real_mount(struct vfsmount *mnt)
{
	return container_of(mnt, struct mount, mnt);
}

static inline int mnt_has_parent(struct mount *mnt)
{
	return mnt != mnt->mnt_parent;
}

static inline int is_mounted(struct vfsmount *mnt)
{
	/* neither detached nor internal? */
	return !IS_ERR_OR_NULL(real_mount(mnt)->mnt_ns);
}

extern struct mount *__lookup_mnt(struct vfsmount *, struct dentry *, int);

static inline void get_mnt_ns(struct mnt_namespace *ns)
{
	atomic_inc(&ns->count);
}
//�󲿷ֳ�Ա��mounts_open_common()����show_mountinfo()�и�ֵ
struct proc_mounts {
	struct seq_file m;
	struct mnt_namespace *ns;//�����ռ䣬���Ե�ǰ����task��struct nsproxy��struct mnt_namespace��Ա
	struct path root;	//ָ��ǰ���������ĸ��ļ�ϵͳ
	int (*show)(struct seq_file *, struct vfsmount *);//mounts_open_common��ֵΪshow_vfsmnt
};

#define proc_mounts(p) (container_of((p), struct proc_mounts, m))

extern const struct seq_operations mounts_op;
