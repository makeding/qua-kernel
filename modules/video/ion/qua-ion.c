// SPDX-License-Identifier: GPL-2.0-or-later
/* Experimental Realtek legacy ION bridge; not yet suitable for deployment. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/idr.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fdtable.h>

struct ion_alloc { size_t len, align; u32 heap, flags; int handle; };
struct ion_handle { int handle; };
struct ion_fd { int handle, fd; };
struct ion_custom { u32 cmd; unsigned long arg; };
struct ion_phy { int handle; u64 addr, len; };
struct ion_phys_legacy { int handle; unsigned long addr; unsigned int len; };
#define ION_ALLOC _IOWR('I',0,struct ion_alloc)
#define ION_FREE _IOWR('I',1,struct ion_handle)
#define ION_MAP _IOWR('I',2,struct ion_fd)
#define ION_SHARE _IOWR('I',4,struct ion_fd)
#define ION_CUSTOM _IOWR('I',6,struct ion_custom)
#define ION_PHYS _IOWR('I',9,struct ion_phys_legacy)
#define ION_SYNC _IOWR('I',7,struct ion_fd)

struct qua_ion_buffer { struct device *dev; void *cpu; dma_addr_t dma; size_t size; };
struct qua_ion_client { struct idr handles; struct mutex lock; };
static struct platform_device *ion_dev;

static struct sg_table *buffer_map(struct dma_buf_attachment *attachment, enum dma_data_direction dir)
{
 struct qua_ion_buffer *b = attachment->dmabuf->priv;
 struct sg_table *sgt = kzalloc(sizeof(*sgt), GFP_KERNEL); int ret;
 if (!sgt) return ERR_PTR(-ENOMEM);
 ret = dma_get_sgtable(b->dev, sgt, b->cpu, b->dma, b->size);
 if (ret) { kfree(sgt); return ERR_PTR(ret); }
 ret = dma_map_sgtable(attachment->dev, sgt, dir, 0);
 if (ret) { sg_free_table(sgt); kfree(sgt); return ERR_PTR(ret); }
 return sgt;
}
static void buffer_unmap(struct dma_buf_attachment *attachment, struct sg_table *sgt, enum dma_data_direction dir)
{
 dma_unmap_sgtable(attachment->dev, sgt, dir, 0); sg_free_table(sgt); kfree(sgt);
}
static int buffer_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
 struct qua_ion_buffer *b = dmabuf->priv;
 return dma_mmap_coherent(b->dev, vma, b->cpu, b->dma, b->size);
}
static void buffer_release(struct dma_buf *dmabuf)
{
 struct qua_ion_buffer *b = dmabuf->priv;
 dma_free_coherent(b->dev, b->size, b->cpu, b->dma);
 kfree(b);
}
static const struct dma_buf_ops buffer_ops = { .map_dma_buf = buffer_map, .unmap_dma_buf = buffer_unmap, .mmap = buffer_mmap, .release = buffer_release };
static int ion_open_file(struct inode *inode, struct file *file)
{
 struct qua_ion_client *c = kzalloc(sizeof(*c), GFP_KERNEL);
 if (!c) return -ENOMEM;
 idr_init(&c->handles); mutex_init(&c->lock); file->private_data = c;
 return 0;
}
static int ion_close_file(struct inode *inode, struct file *file)
{
 struct qua_ion_client *c = file->private_data; struct dma_buf *db; int id;
 idr_for_each_entry(&c->handles, db, id) dma_buf_put(db);
 idr_destroy(&c->handles); kfree(c); return 0;
}
static long ion_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
 struct qua_ion_client *c = file->private_data; void __user *u = (void __user *)arg;
 struct dma_buf *db; struct qua_ion_buffer *b; int id, ret = 0;
 mutex_lock(&c->lock);
 switch (cmd) {
 case ION_ALLOC: {
  struct ion_alloc a; DEFINE_DMA_BUF_EXPORT_INFO(exp);
  if (copy_from_user(&a, u, sizeof(a))) { ret = -EFAULT; break; }
  pr_info("qua-ion alloc len=%zu align=%zu heap=%x flags=%x\n", a.len, a.align, a.heap, a.flags);
  if (a.heap != BIT(7) || (a.flags & (BIT(23) | BIT(25) | BIT(26)))) { ret = -EOPNOTSUPP; break; }
  if (!a.len || a.len > SZ_32M || a.align > PAGE_SIZE) { ret = -EINVAL; break; }
  b = kzalloc(sizeof(*b), GFP_KERNEL); if (!b) { ret = -ENOMEM; break; }
  b->dev = &ion_dev->dev; b->size = PAGE_ALIGN(a.len);
  b->cpu = dma_alloc_coherent(b->dev, b->size, &b->dma, GFP_KERNEL | GFP_DMA);
  if (!b->cpu) { kfree(b); ret = -ENOMEM; break; }
  exp.ops = &buffer_ops; exp.size = b->size; exp.flags = O_RDWR; exp.priv = b;
  db = dma_buf_export(&exp);
  if (IS_ERR(db)) { ret = PTR_ERR(db); dma_free_coherent(b->dev,b->size,b->cpu,b->dma); kfree(b); break; }
  id = idr_alloc(&c->handles, db, 1, 0, GFP_KERNEL);
  if (id < 0) { dma_buf_put(db); ret = id; break; }
  a.handle = id;
  if (copy_to_user(u, &a, sizeof(a))) { idr_remove(&c->handles,id); dma_buf_put(db); ret = -EFAULT; }
  break;
 }
 case ION_FREE: {
  struct ion_handle h;
  if (copy_from_user(&h,u,sizeof(h))) { ret = -EFAULT; break; }
  db = idr_remove(&c->handles,h.handle); if (!db) { ret = -ENOENT; break; }
  dma_buf_put(db); break;
 }
 case ION_MAP: case ION_SHARE: {
  struct ion_fd f;
  if (copy_from_user(&f,u,sizeof(f))) { ret = -EFAULT; break; }
  db = idr_find(&c->handles,f.handle); if (!db) { ret = -ENOENT; break; }
  get_dma_buf(db); f.fd = dma_buf_fd(db,O_CLOEXEC);
  if (f.fd < 0) { dma_buf_put(db); ret = f.fd; break; }
  if (copy_to_user(u,&f,sizeof(f))) { close_fd(f.fd); ret = -EFAULT; }
  break;
 }
 case ION_PHYS: {
  struct ion_phys_legacy p;
  if (copy_from_user(&p,u,sizeof(p))) { ret = -EFAULT; break; }
  db = idr_find(&c->handles,p.handle); if (!db) { ret = -ENOENT; break; }
  b = db->priv; p.addr = b->dma; p.len = b->size;
  if (copy_to_user(u,&p,sizeof(p))) ret = -EFAULT;
  break;
 }
 case ION_CUSTOM: {
  struct ion_custom x; struct ion_phy p;
  struct dma_buf *sync_buf;
  if (copy_from_user(&x,u,sizeof(x))) { ret = -EFAULT; break; }
  if (x.cmd == 0x10 || x.cmd == 0x11) {
   sync_buf = dma_buf_get((int)x.arg);
   if (IS_ERR(sync_buf)) { ret = PTR_ERR(sync_buf); break; }
   if (sync_buf->ops != &buffer_ops) ret = -EINVAL;
   dma_buf_put(sync_buf); break;
  }
  if (x.cmd != 0x15) { pr_info("qua-ion unsupported custom=%#x\n", x.cmd); ret = -EOPNOTSUPP; break; }
  if (copy_from_user(&p,(void __user *)x.arg,sizeof(p))) { ret = -EFAULT; break; }
  db = idr_find(&c->handles,p.handle); if (!db) { ret = -ENOENT; break; }
  b = db->priv; p.addr = b->dma; p.len = b->size;
  if (copy_to_user((void __user *)x.arg,&p,sizeof(p))) ret = -EFAULT;
  break;
 }
 case ION_SYNC: ret = 0; break;
 default: ret = -ENOTTY;
 }
 mutex_unlock(&c->lock); return ret;
}
static const struct file_operations ion_fops = { .owner=THIS_MODULE, .open=ion_open_file, .release=ion_close_file, .unlocked_ioctl=ion_ioctl };
static struct miscdevice ion_misc = { .minor=MISC_DYNAMIC_MINOR, .name="ion", .fops=&ion_fops, .mode=0600 };
static int __init qua_ion_init(void)
{
 int ret;
 ion_dev = platform_device_register_simple("qua-ion", PLATFORM_DEVID_NONE, NULL, 0);
 if (IS_ERR(ion_dev)) return PTR_ERR(ion_dev);
 ret = dma_coerce_mask_and_coherent(&ion_dev->dev, DMA_BIT_MASK(30));
 if (!ret) ret = misc_register(&ion_misc);
 if (ret) platform_device_unregister(ion_dev);
 return ret;
}
static void __exit qua_ion_exit(void) { misc_deregister(&ion_misc); platform_device_unregister(ion_dev); }
module_init(qua_ion_init); module_exit(qua_ion_exit);
MODULE_LICENSE("GPL"); MODULE_IMPORT_NS("DMA_BUF");
