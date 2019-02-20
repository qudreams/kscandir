#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/mount.h>
#include <linux/syscalls.h>
#include <linux/version.h>
#include <linux/vermagic.h>

/*
 *Note:
 *1. struct path don't defined before linux-2.6.20.
 *2. struct path is defined in linux/namei.h before linux-2.6.25
 *3. struct path is defined in linux/path.h after 2.6.25
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,25)
#include <linux/path.h> //for struct path
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
struct path {
    struct vfsmount *mnt;
    struct dentry *dentry;
};
#endif


struct dent_node_t {
    struct list_head node;
    char* d_name;
    int namelen;
    unsigned int d_type;
};


//struct dir_context must be the first memeber
struct getdents_callback_t {
#if LINUX_VERSION_CODE > KERNEL_VERSION(3,10,0)
    struct dir_context ctx;
#endif
    struct list_head* dents;
    char* root;
    int rootlen;
    int count;
    int error;
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,39)
void nameidata_to_path(struct nameidata* nd,struct path* path)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,25)
    path->mnt = nd->mnt;
    path->dentry = nd->dentry;
#else
    *path = nd->path;
#endif
}
#endif

//kernel path lookup
static int kpath_lookup(const char* pathname,
				unsigned int flags,struct path* path)
{
    int rc = -ENOENT;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 39)
    struct nameidata nd;
    rc = path_lookup(pathname,flags,&nd);
    if(!rc) {
        nameidata_to_path(&nd,path);
    }
#else
    rc = kern_path(pathname,flags,path);
#endif

    return rc;
}

static void kpath_put(struct path* path)
{
    dput(path->dentry);
    mntput(path->mnt);
}

static int kvfs_getattr(struct path* path,struct kstat* stat)
{
    int rc = -EINVAL;
    if(!path || !stat) { return rc; }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,9,0)
    rc = vfs_getattr(path,stat);
#else
    rc = vfs_getattr(path->mnt,path->dentry,stat);
#endif

    return rc;
}

static char dir_root[128] = {0};

static void free_dents(struct list_head* dents);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18)
static int filldir(void * __buf, const char * name, int namlen, loff_t offset,
		   ino_t ino, unsigned int d_type)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3,19,0)
static int filldir(void * __buf, const char * name, int namlen, loff_t offset,
		                  u64 ino, unsigned int d_type)
#else
static int filldir(struct dir_context *ctx, const char * name, int namlen, loff_t offset,
		                  u64 ino, unsigned int d_type)
#endif
{
    size_t len = 0;
    char* dname = NULL;
    struct dent_node_t* dnode = NULL;
    #if LINUX_VERSION_CODE < KERNEL_VERSION(3,19,0)
    struct getdents_callback_t* buf = (struct getdents_callback_t*)__buf;
    #else
    struct getdents_callback_t* buf = container_of(ctx, struct getdents_callback_t, ctx);
    #endif

    if((!strcmp("..",name)) || (!strcmp(".",name))) {
        return 0;
    }

    // //we just care directory and regular file
    if((d_type != DT_REG) && (d_type != DT_DIR)) {
        return 0;
    }

    dnode = kcalloc(1,sizeof(*dnode),GFP_KERNEL);
    if(!dnode) {
        buf->error = -ENOMEM;
        goto fault;
    }

    len = buf->rootlen + namlen + sizeof("/") - 1;
    dname = kcalloc(1,len + 1,GFP_KERNEL);
    if(!dname) {
       buf->error = -ENOMEM;
       goto fault;
    }
    len = snprintf(dname,len + 1,"%s/%s",buf->root,name);

    buf->count++;
    dnode->d_name = dname;
    dnode->namelen = len;
    dnode->d_type = d_type;
    INIT_LIST_HEAD(&dnode->node);
    list_add_tail(&dnode->node,buf->dents);

    return 0;

fault:
    if(dnode) { kfree(dnode); }

    return buf->error;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,23)
/**
 * kstrndup - allocate space for and copy an existing string
 * @s: the string to duplicate
 * @max: read at most @max chars from @s
 * @gfp: the GFP mask used in the kmalloc() call when allocating memory
 */
char *kstrndup(const char *s, size_t max, gfp_t gfp)
{
    size_t len;
    char *buf;

    if (!s)
        return NULL;

    len = strnlen(s, max);
    buf = kcalloc(1,len+1, gfp);
    if (buf) {
        memcpy(buf, s, len);
        buf[len] = '\0';
    }
    return buf;
}
#endif

//return the number of files,and return errno if an error occured
int scan_dir(const char* dname,struct list_head* dents)
{
    int rc = -EINVAL;
    struct file* filp = NULL;
    struct getdents_callback_t buf = {
#if LINUX_VERSION_CODE > KERNEL_VERSION(3,10,0)
        .ctx.actor = filldir,
#endif
        .root = NULL,
        .rootlen = 0,
        .dents = dents,
        .count = 0,
        .error = 0
    };

    if((!dname) || (!dents)) { goto out; }

    rc = -ENOENT;
    filp = filp_open(dname,O_RDONLY|O_NONBLOCK|O_DIRECTORY,0);
    if(IS_ERR(filp)) { goto out; }

    buf.rootlen = strlen(dname);
    buf.root = kstrndup(dname,buf.rootlen,GFP_KERNEL);
    if(!(buf.root)) { goto out; }

#if LINUX_VERSION_CODE > KERNEL_VERSION(3,10,0)
    rc = iterate_dir(filp,&buf.ctx);
#else
    rc = vfs_readdir(filp,filldir,&buf);
#endif
    if(rc) { free_dents(dents); }

out:
    if(filp) { filp_close(filp,NULL); }
    if(buf.root) { kfree(buf.root); }
    if(!rc) { rc = buf.count; }

    return rc;
}

void free_dent_node(struct dent_node_t* dnode)
{
    if(dnode->d_name) { kfree(dnode->d_name); }
    kfree(dnode);
}

void free_dents(struct list_head* dents)
{
    struct dent_node_t* next = NULL;
    struct dent_node_t* dnode = NULL;

    list_for_each_entry_safe(dnode,next,dents,node)
    {
        list_del(&dnode->node);
        free_dent_node(dnode);
    }
}

static int may_dir(const char* pathname)
{
    int rc = 0;
    struct path path;
    struct kstat stat;

    rc = kpath_lookup(pathname,LOOKUP_FOLLOW,&path);
    if(rc) { return rc; }

    rc = kvfs_getattr(&path,&stat);
    kpath_put(&path);
    if(rc) { return rc; }

    if(!S_ISDIR(stat.mode)) { rc = -EINVAL; }
    return rc;
}

static int scan_dir_recursive(const char* dir)
{
    int dtype = 0;
    size_t len = 0;
    int rc = -EINVAL;
    struct list_head dents;
    struct list_head* phead = NULL;
    struct dent_node_t* dnode = NULL;

    if(unlikely(!dir)) { return rc; }

    rc = may_dir(dir);
    if(rc) { return rc; }

    rc = -ENOMEM;
    dnode = kcalloc(1,sizeof(*dnode),GFP_KERNEL);
    if(!dnode) { return rc; }

    len = strlen(dir);
    dnode->d_name = kstrndup(dir,len,GFP_KERNEL);
    if(!dnode->d_name) {
    	kfree(dnode);
    	return rc;
    }

    dnode->namelen = len;
    dnode->d_type = DT_DIR;
    INIT_LIST_HEAD(&dnode->node);
    INIT_LIST_HEAD(&dents);

    list_add_tail(&dnode->node,&dents);

    rc = 0;
    while(!list_empty(&dents)) {
        phead = dents.next;
        dnode = list_entry(phead,struct dent_node_t,node);
        list_del(&dnode->node);

        printk("fill dir ent: %s\n",dnode->d_name);

        dtype = dnode->d_type;
        if((dtype != DT_DIR) && (dtype != DT_REG)) {
            free_dent_node(dnode);
            continue;
        }

        if(dtype == DT_DIR) { (void)scan_dir(dnode->d_name,&dents); }
        free_dent_node(dnode);
    }

    free_dents(&dents);
    return rc;
}

#define DEVICE_NAME     "kscandir"

static int __init kscandir_init(void)
{
    int rc = 0;
    printk("-----Start kscandir: %s,"
        "kernel-version: %s\n",dir_root,UTS_RELEASE);

    rc = scan_dir_recursive(dir_root);

    return rc;
}

static void __exit kscandir_exit(void)
{
    printk("-----Exit kscandir-----\n");
}

module_param_string(dir_root,dir_root,sizeof(dir_root),0);
MODULE_PARM_DESC(dir_root, "scan the directory in kernel");

module_init(kscandir_init);
module_exit(kscandir_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("qudreams");
MODULE_DESCRIPTION(DEVICE_NAME);
MODULE_VERSION(DEVICE_VERSION);
