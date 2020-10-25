#ifndef _LINUX_PATH_H
#define _LINUX_PATH_H

struct dentry;
struct vfsmount;

//walk_component->lookup_fast()������ϸע��
struct path {
    //�����������ļ�����Ŀ¼���ڵ��ļ�ϵͳ��mount�ṹ���vfsmount�ṹ��
    //mount����ʱ����ѹ��ص�Ŀ¼ת���ɹ���Դ�ģ��ʴ�ʱ���ϴι���Դ���豸�ļ�ϵͳ��vfsmount
	struct vfsmount *mnt;
	//���Ǳ�����������Ŀ¼�����ļ���dentry����mount����ʱ����ѹ��ص�Ŀ¼ת���ɹ���Դ�ģ��ʴ�ʱ����Դ���豸�ĸ�Ŀ¼��
	struct dentry *dentry;
};

extern void path_get(const struct path *);
extern void path_put(const struct path *);

static inline int path_equal(const struct path *path1, const struct path *path2)
{
	return path1->mnt == path2->mnt && path1->dentry == path2->dentry;
}

#endif  /* _LINUX_PATH_H */
