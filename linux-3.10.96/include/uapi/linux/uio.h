/*
 *	Berkeley style UIO structures	-	Alan Cox 1994.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _UAPI__LINUX_UIO_H
#define _UAPI__LINUX_UIO_H

#include <linux/compiler.h>
#include <linux/types.h>

//���iovec��ϵͳ����read/write�Ĵ������ݼ�ǿ�档��ͨwrite/read��write(int fd, const void *buf...)ֻ��ָ��һ��buf��
//writev/readv��writev(int fd, const struct iovec *iov, int iovcnt)��ʹ��iovec�����Ƭ�ڴ��е����ݣ�iovcntָ��iovec�ĸ���
//ÿһ��iovec��iov_base���û��ռ�buf�׵�ַ��iov_len�ǳ��ȡ�˵���ˣ�writev����ͨ��iovec���δ����Ƭ�ڴ�����ݶ��ѣ�������·����
struct iovec
{
    //read/write�û��ռ䴫�ݵ�buf��do_sync_read()
	void __user *iov_base;	/* BSD uses caddr_t (1003.1g requires void *) */
    //read/write ��ȡ���ֽ�����do_sync_read()
	__kernel_size_t iov_len; /* Must be size_t (1003.1g) */
};

/*
 *	UIO_MAXIOV shall be at least 16 1003.1g (5.4.1.1)
 */
 
#define UIO_FASTIOV	8
#define UIO_MAXIOV	1024


#endif /* _UAPI__LINUX_UIO_H */
