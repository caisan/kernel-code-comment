#ifndef _LINUX_PATH_H
#define _LINUX_PATH_H

struct dentry;
struct vfsmount;

struct path {
    //�����������ļ�����Ŀ¼���ڵ��ļ�ϵͳ��mount�ṹ���vfsmount�ṹ
	struct vfsmount *mnt;
	//���Ǳ�����������Ŀ¼�����ļ���dentry����mount����ʱ���ǹ��ص�Ŀ¼��dentry
	struct dentry *dentry;
};

extern void path_get(const struct path *);
extern void path_put(const struct path *);

static inline int path_equal(const struct path *path1, const struct path *path2)
{
	return path1->mnt == path2->mnt && path1->dentry == path2->dentry;
}

#endif  /* _LINUX_PATH_H */
