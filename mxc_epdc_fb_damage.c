// For KDevelop's sake...
#ifndef __KERNEL__
#	define __KERNEL__
#endif

#include <linux/cdev.h>
#include <linux/circ_buf.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "mxc_epdc_fb_damage.h"

// Prefer READ_ONCE if it's available (in which case, ACCESS_ONCE is liable to be gone, too).
// c.f., https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=01e4644203b01fba5023784598f4d033e3bd3e28
#ifdef READ_ONCE
#	ifdef ACCESS_ONCE
#		undef ACCESS_ONCE
#		define ACCESS_ONCE READ_ONCE
#	endif
#endif

static int fbnode = 0;
module_param(fbnode, int, 0);

static atomic_t overflows = ATOMIC_INIT(0);

#define NUM_UPD_BUF 64
/* For simplicity in read() store mxcfb_damage_update rather than mxcfb_update_data.
 *
 * The overflow_notify field is populated by read() and hence usually useless.
 */
struct mxcfb_damage_update upd_data[NUM_UPD_BUF];
unsigned long              upd_buf_head = 0;
unsigned long              upd_buf_tail = NUM_UPD_BUF - 1;
DECLARE_WAIT_QUEUE_HEAD(listen_queue);

static int (*orig_fb_ioctl)(struct fb_info* info, unsigned int cmd, unsigned long arg);

static int
    fb_ioctl(struct fb_info* info, unsigned int cmd, unsigned long arg)
{
	int ret = orig_fb_ioctl(info, cmd, arg);
	if (cmd == MXCFB_SEND_UPDATE) {
		/* The fb_ioctl() is called with the console lock held, so there is no need for additional locking here */
		unsigned long head = upd_buf_head;
		unsigned long tail = ACCESS_ONCE(upd_buf_tail);
		if (CIRC_SPACE(head, tail, NUM_UPD_BUF) >= 1) {
			(void) !copy_from_user(
			    &upd_data[head].data, (void __user*) arg, sizeof(struct mxcfb_update_data));
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0)
			smp_store_release(&upd_buf_head, (head + 1) & (NUM_UPD_BUF - 1));
#else
			smp_wmb(); /* commit the item before incrementing the head */
			ACCESS_ONCE(upd_buf_head) = (head + 1) & (NUM_UPD_BUF - 1);
#endif
		} else {
			atomic_inc(&overflows);
		}
		wake_up(&listen_queue);
	}
	return ret;
}

static atomic_t readers = ATOMIC_INIT(0);

static int
    fbdamage_open(struct inode* inode, struct file* filp)
{
	if (atomic_xchg(&readers, 1)) {
		return -ENODEV;
	}
	return 0;
}
static int
    fbdamage_release(struct inode* inode, struct file* filp)
{
	atomic_xchg(&readers, 0);
	return 0;
}

static ssize_t
    fbdamage_read(struct file* filp, char __user* buff, size_t count, loff_t* offp)
{
	unsigned long head, tail;
	if (count < sizeof(struct mxcfb_damage_update)) {
		return -EINVAL;
	}
	/* no need for locks, since we only allow one reader */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0)
	head = smp_load_acquire(&upd_buf_head);
#else
	head = ACCESS_ONCE(upd_buf_head);
#endif
	tail = upd_buf_tail;
	while (!CIRC_CNT(head, tail, NUM_UPD_BUF)) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0)
		if (wait_event_interruptible(listen_queue,
					     (CIRC_CNT(smp_load_acquire(&upd_buf_head), upd_buf_tail, NUM_UPD_BUF)))) {
#else
		if (wait_event_interruptible(listen_queue,
					     (CIRC_CNT(ACCESS_ONCE(upd_buf_head), upd_buf_tail, NUM_UPD_BUF)))) {
#endif
			return -ERESTARTSYS;
		}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0)
		head = smp_load_acquire(&upd_buf_head);
#else
		head = ACCESS_ONCE(upd_buf_head);
#endif
		tail = upd_buf_tail;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
	/* read index before reading contents at that index */
	smp_rmb();
#endif

	upd_data[tail].overflow_notify = atomic_xchg(&overflows, 0);
	if (copy_to_user(buff, &upd_data[tail], sizeof(struct mxcfb_damage_update))) {
		return -EFAULT;
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0)
	smp_store_release(&upd_buf_tail, (tail + 1) & (NUM_UPD_BUF - 1));
#else
	smp_mb(); /* finish reading descriptor before incrementing tail */
	ACCESS_ONCE(upd_buf_tail) = (tail + 1) & (NUM_UPD_BUF - 1);
#endif
	return sizeof(struct mxcfb_damage_update);
}

static dev_t                        dev;
static struct class*                fbdamage_class;
static struct device*               fbdamage_device;
static struct cdev                  cdev;
static const struct file_operations fbdamage_fops = { .owner   = THIS_MODULE,
						      .open    = fbdamage_open,
						      .read    = fbdamage_read,
						      .release = fbdamage_release };

int
    init_module(void)
{
	int ret;
	if (!registered_fb[fbnode]) {
		return -ENODEV;
	}
	orig_fb_ioctl                          = registered_fb[fbnode]->fbops->fb_ioctl;
	registered_fb[fbnode]->fbops->fb_ioctl = fb_ioctl;
	if ((ret = alloc_chrdev_region(&dev, 0, 1, "mxc_epdc_fb_damage"))) {
		return ret;
	}
	cdev_init(&cdev, &fbdamage_fops);
	if ((ret = cdev_add(&cdev, dev, 1) < 0)) {
		unregister_chrdev_region(dev, 1);
		return ret;
	}
	fbdamage_class  = class_create(THIS_MODULE, "fbdamage");
	fbdamage_device = device_create(fbdamage_class, NULL, dev, NULL, "fbdamage");
	return 0;
}

void
    cleanup_module(void)
{
	cdev_del(&cdev);
	device_destroy(fbdamage_class, dev);
	class_destroy(fbdamage_class);
	unregister_chrdev_region(dev, 1);

	registered_fb[fbnode]->fbops->fb_ioctl = orig_fb_ioctl;
}

MODULE_LICENSE("GPL");
