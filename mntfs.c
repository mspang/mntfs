// Copyright (C) 2011 Michael Spang

#include <linux/module.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/nsproxy.h>
#include <linux/namei.h>
#include <linux/mnt_namespace.h>
#include <linux/mount.h>
#include <linux/slab.h>

static struct inode *mntfs_iget(struct super_block *sb, unsigned long ino);

static int mnt_inode(struct vfsmount *mnt) {
	return mnt->mnt_id + 1000;
}

static int mntfs_dentry_delete(const struct dentry *dentry) {
	return 1;
}

static const struct dentry_operations mntfs_dentry_operations = {
	.d_delete = mntfs_dentry_delete,
};

static struct vfsmount *find_mount_by_id(int id) {
	struct nsproxy *nsp = NULL;
	struct mnt_namespace *ns = NULL;
	struct vfsmount *mnt;

	nsp = task_nsproxy(current);
	if (nsp)
		ns = nsp->mnt_ns;
	if (!ns)
		return ERR_PTR(-ENOENT);

	// racy
	list_for_each_entry(mnt, &ns->list, mnt_list) {
		if (mnt->mnt_id == id) {
			mntget(mnt);
			return mnt;
		}
	}

	return ERR_PTR(-ENOENT);
}

static struct vfsmount *find_mount_by_dentry(struct dentry *dentry) {
	struct vfsmount *mnt;
	unsigned long id;

        if (dentry->d_name.len > NAME_MAX)
                return ERR_PTR(-ENAMETOOLONG);

	if (dentry->d_name.name[0] == '0' && dentry->d_name.len > 1)
		return ERR_PTR(-ENOENT);

	if (strict_strtoul(dentry->d_name.name, 10, &id))
		return ERR_PTR(-ENOENT);

	mnt = find_mount_by_id(id);
	if (IS_ERR(mnt))
		return ERR_CAST(mnt);

	return mnt;
}

static struct dentry *mntfs_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *nd)
{
	struct inode *inode;
	struct vfsmount *mnt;

	mnt = find_mount_by_dentry(dentry);
	if (IS_ERR(mnt))
		return ERR_CAST(mnt);

	inode = mntfs_iget(dir->i_sb, mnt_inode(mnt));
	d_add(dentry, inode);

	mntput(mnt);
	return NULL;
}

static const struct inode_operations mntfs_root_inode_operations = {
        .lookup = mntfs_lookup,
};

static int mntfs_readdir(struct file * filp,
		          void * dirent, filldir_t filldir) {
	struct nsproxy *nsp = NULL;
	struct mnt_namespace *ns = NULL;
	struct vfsmount *mnt;
	int i = 0;

	nsp = task_nsproxy(current);
	if (nsp)
		ns = nsp->mnt_ns;
	if (!ns)
		goto out;

	// racy
	list_for_each_entry(mnt, &ns->list, mnt_list) {
		if (filp->f_pos <= i) {
			char name[13];
			int len = snprintf(name, sizeof(name), "%d", mnt->mnt_id);
			filldir(dirent, name, len, filp->f_pos, mnt_inode(mnt), DT_LNK);
			filp->f_pos = i + 1;
		}
		i++;
	}

out:
	return 0;
}

static const struct file_operations mntfs_root_file_operations = {
	.read = generic_read_dir,
	.readdir = mntfs_readdir,
	.llseek = default_llseek,
};

static int mntfs_readlink(struct dentry *dentry, char __user *buffer,
                              int buflen) {
	struct vfsmount *mnt;
	struct path path;
	char *namebuf;
	char *name;
	int error = -ENOENT;

	mnt = find_mount_by_dentry(dentry);
	if (IS_ERR(mnt))
		return PTR_ERR(mnt);

	namebuf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!namebuf)
		goto out_mntput;

	path.mnt = mnt;
	path.dentry = mnt->mnt_root;

	name = d_path(&path, namebuf, PATH_MAX);
	if (IS_ERR(name))
		goto out_free;

        error = vfs_readlink(dentry, buffer, buflen, name);

out_free:
	kfree(namebuf);
out_mntput:
	mntput(mnt);
	return error;
}

static void *mntfs_follow(struct dentry *dentry, struct nameidata *nd) {
	struct vfsmount *mnt;

	path_put(&nd->path);

	mnt = find_mount_by_dentry(dentry);
	if (IS_ERR(mnt))
		return ERR_CAST(mnt);

	nd->path.mnt = mnt;
	nd->path.dentry = mnt->mnt_root;
	path_get(&nd->path);

	mntput(mnt);

	return NULL;
}

static const struct inode_operations mntfs_link_inode_operations = {
	.readlink = mntfs_readlink,
	.follow_link = mntfs_follow,
};

static struct inode *mntfs_iget(struct super_block *sb, unsigned long ino) {
	struct inode *inode;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

        inode->i_nlink = 1;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;

	if (ino == 1) {
		inode->i_mode = S_IFDIR | S_IRUGO | S_IXUGO;
		inode->i_fop = &mntfs_root_file_operations;
		inode->i_op = &mntfs_root_inode_operations;
	} else {
		inode->i_mode = S_IFLNK | S_IRWXUGO;
		inode->i_op = &mntfs_link_inode_operations;
	}

	unlock_new_inode(inode);
	return inode;
}

static struct super_operations mntfs_super_operations = {
	.statfs = simple_statfs,
};

static int mntfs_fill_super(struct super_block *sb, void *data, int silent) {
        struct inode *root;

	sb->s_op = &mntfs_super_operations;

	root = mntfs_iget(sb, 1);
	if (IS_ERR(root))
		return PTR_ERR(root);

	sb->s_root = d_alloc_root(root);
	sb->s_d_op = &mntfs_dentry_operations;
	if (!sb->s_root)
		return -ENOMEM;

        return 0;
}

static struct dentry *mntfs_mount(struct file_system_type *fs_type,
		int flags, const char *dev_name, void *data) {
	return mount_nodev(fs_type, flags, data, mntfs_fill_super);
}

static struct file_system_type mntfs_type = {
        .name = "mntfs",
        .mount = mntfs_mount,
        .kill_sb = kill_anon_super,
        .owner = THIS_MODULE,
};

static int __init mntfs_init(void) {
	register_filesystem(&mntfs_type);
	return 0;
}

static void __exit mntfs_exit(void) {
	unregister_filesystem(&mntfs_type);
}

module_init(mntfs_init);
module_exit(mntfs_exit);

MODULE_AUTHOR("Michael Spang");
MODULE_LICENSE("Dual BSD/GPL");
