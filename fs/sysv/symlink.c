/*
 *  linux/fs/sysv/symlink.c
 *
 *  Handling of System V filesystem fast symlinks extensions.
 *  Aug 2001, Christoph Hellwig (hch@infradead.org)
 */

#include "sysv.h"
#include <linux/namei.h>

static void *sysv_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	struct inode *inode = dentry->d_inode;
	if (!inode)
		return ERR_PTR(-ECHILD);
	nd_set_link(nd, (char *)SYSV_I(inode)->i_data);
	return NULL;
}

const struct inode_operations sysv_fast_symlink_inode_operations = {
	.readlink	= generic_readlink,
	.follow_link_rcu = sysv_follow_link,
};
