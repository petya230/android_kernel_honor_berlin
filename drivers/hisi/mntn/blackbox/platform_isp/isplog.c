/*
 * record the data to rdr. (RDR: kernel run data recorder.)
 * This file wraps the ring buffer.
 *
 * Copyright (c) 2013 Hisilicon Technologies CO., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
/*lint -e715 -e838 -e529 -e438 -e30 -e142 -e528 -e750 -e753 -e754 -e785 -e655 -e749 -e732 -e747 -e708 -e712 -e64 -e845 -e713 -e40 -e578 -e774
 -esym(715,*) -esym(838,*) -esym(529,*) -esym(438,*) -esym(30,*) -esym(142,*) -esym(528,*) -esym(750,*) -esym(753,*) -esym(754,*) -esym(785,*) -esym(655,*) -esym(749,*) -esym(732,*) -esym(747,*) -esym(708,*) -esym(712,*) -esym(64,*) -esym(845,*) -esym(713,*) -esym(40,*) -esym(578,*) -esym(774,*)*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/rproc_share.h>
#include <linux/remoteproc.h>
#include <linux/atomic.h>
#include <linux/io.h>
#include <linux/platform_data/remoteproc-hisi.h>
#include "isprdr.h"

#define MEM_MAP_MAX_SIZE    (0x40000)
#define MEM_SHARED_SIZE     (0x1000)
#define ISP_IOCTL_MAGIC     (0x70)
#define ISP_IOCTL_MAX_NR    (0x10)
#define POLLING_TIME_NS     (10)
#define POLLING_TIMEOUT_NS  (400)

struct log_user_para {
    unsigned int log_write;
    unsigned int log_head_size;
};

#define LOG_WR_OFFSET       _IOWR(ISP_IOCTL_MAGIC, 0x00, struct log_user_para)

struct cdev_info {
	dev_t devt;
	struct cdev log_chrdev;
	struct device *log_dev;
	struct class *log_dev_class;
};

struct isplog_device_s {
    void __iomem *share_mem;
    struct rproc_shared_para *share_para;
    wait_queue_head_t wait_ctl;
    struct timer_list sync_timer;
    atomic_t open_cnt;
    int use_cacheable_rdr;
    int initialized;
    atomic_t timer_cnt;
    struct cdev_info log_cdev;
} isplog_dev;

int sync_isplogcat(void)
{
    struct isplog_device_s *dev = (struct isplog_device_s *)&isplog_dev;

    if (!dev->initialized) {
        pr_err("[%s] Failed : ISP RDR not ready\n", __func__);
        return -ENXIO;
    }

    if (use_sec_isp())
        dev->share_mem = (void *)getsec_a7sharedmem_addr();
    else if (use_nonsec_isp())
        dev->share_mem = get_a7sharedmem_va();

    if (!dev->share_mem) {
        pr_err("[%s] Failed: share_mem.%pK\n", __func__, dev->share_mem);
        return -ENOMEM;
    }
    dev->share_para = (struct rproc_shared_para *)dev->share_mem;

    if(dev->share_para->log_flush_flag)
        wake_up(&dev->wait_ctl);

    return 0;
}

static void sync_timer_fn(unsigned long data)
{
    struct isplog_device_s *dev = (struct isplog_device_s *)&isplog_dev;

    pr_debug("sync_timer_fn enter.%pK\n", dev->share_para);

    if (sync_isplogcat() < 0)
        pr_err("[%s] Failed: sync_isplogcat.%pK\n", __func__, dev->share_para);

	mod_timer(&dev->sync_timer, jiffies + msecs_to_jiffies(POLLING_TIME_NS));
}
/*lint -save -e529 -e438*/
void stop_isplogcat(void)
{
    struct isplog_device_s *dev = (struct isplog_device_s *)&isplog_dev;

    pr_info("[%s] +\n", __func__);
    if (!dev->initialized) {
        pr_err("[%s] Failed : ISP RDR not ready\n", __func__);
        return;
    }

	if (0 == atomic_read(&dev->open_cnt)) {
		pr_err("[%s] Failed : device not ready open_cnt.%d\n", __func__, atomic_read(&dev->open_cnt));
		return;
	}

    if (0 == atomic_read(&dev->timer_cnt)) {
        pr_err("[%s] Failed : timer_cnt.%d...Nothing todo\n", __func__, atomic_read(&dev->timer_cnt));
        return;
    }

    del_timer_sync(&dev->sync_timer);
    atomic_set(&dev->timer_cnt, 0);
    if (sync_isplogcat() < 0)
        pr_err("[%s] Failed: sync_isplogcat\n", __func__);
    pr_info("[%s] -\n", __func__);
}

int start_isplogcat(void)
{
    struct isplog_device_s *dev = (struct isplog_device_s *)&isplog_dev;

    pr_info("[%s] +\n", __func__);
    if (!dev->initialized) {
        pr_err("[%s] Failed : ISP RDR not ready\n", __func__);
        return -ENXIO;
    }

	if (0 == atomic_read(&dev->open_cnt)) {
		pr_err("[%s] Failed : device not ready open_cnt.%d\n", __func__, atomic_read(&dev->open_cnt));
		return -ENODEV;
	}

    if (0 != atomic_read(&dev->timer_cnt)) {
        pr_err("[%s] Failed : timer_cnt.%d...stop isplogcat\n", __func__, atomic_read(&dev->timer_cnt));
        stop_isplogcat();
    }

    mod_timer(&dev->sync_timer, jiffies + msecs_to_jiffies(POLLING_TIME_NS));
    atomic_set(&dev->timer_cnt, 1);
    pr_info("[%s] -\n", __func__);

    return 0;
}

static int isplog_open(struct inode *inode, struct file *filp)
{
    struct isplog_device_s *dev = (struct isplog_device_s *)&isplog_dev;

    pr_info("[%s] +\n", __func__);

    if (!dev->initialized) {
        pr_err("[%s] Failed : ISP RDR not ready\n", __func__);
        return -ENXIO;
    }

    if (0 != atomic_read(&dev->open_cnt)) {
        pr_err("%s: Failed: has been opened\n", __func__);
        return -EBUSY;
    }

    if (!use_sec_isp() && !use_nonsec_isp()) {
        dev->share_mem = ioremap_nocache(get_a7sharedmem_addr(), MEM_SHARED_SIZE);
        if (!dev->share_mem) {
            pr_err("[%s] Failed: share_mem.%pK\n", __func__, dev->share_mem);
            return -ENOMEM;
        }
        dev->share_para = (struct rproc_shared_para *)dev->share_mem;
        pr_info("[%s] use_sec_isp.%d, share_para.%pK = %pK\n", __func__,
            use_sec_isp(), dev->share_para, dev->share_mem);
    }

	atomic_inc(&dev->open_cnt);
	if (is_ispcpu_powerup())
	    start_isplogcat();
	pr_info("[%s] -\n", __func__);

    return 0;
}

static long isplog_ioctl(struct file *filp, unsigned int cmd,
              unsigned long args)
{
    struct isplog_device_s *dev = (struct isplog_device_s *)&isplog_dev;
    struct log_user_para tmp;
    int ret;

    pr_debug("[%s] +\n", __func__);
    if (!dev->initialized) {
        pr_err("[%s] Failed : ISP RDR not ready\n", __func__);
        return -ENXIO;
    }

    if (_IOC_TYPE(cmd) != ISP_IOCTL_MAGIC) {
        pr_err("[%s] type is wrong.\n", __func__);
        return -EINVAL;
    }

    if (_IOC_NR(cmd) >= ISP_IOCTL_MAX_NR) {
        pr_err("[%s] number is wrong.\n", __func__);
        return -EINVAL;
    }


    if (!dev->share_para) {
        if (sync_isplogcat() < 0)
            pr_err("[%s] Failed: sync_isplogcat.%pK\n", __func__, dev->share_para);
        pr_err("[%s] Failed : share_para.%pK\n", __func__, dev->share_para);
        return -EAGAIN;
    }

    switch (cmd) {
    case LOG_WR_OFFSET:
        ret = wait_event_timeout(dev->wait_ctl,
                   dev->share_para->log_flush_flag,
			       msecs_to_jiffies(POLLING_TIMEOUT_NS));//lint !e666
        if (0 == ret) {
            pr_debug("[%s] wait timeout, ret = %d\n", __func__, ret);
            return -ETIMEDOUT;
        }

		if (dev->use_cacheable_rdr)
			dev->share_para->log_flush_flag = 0;

		tmp.log_write = dev->share_para->log_cache_write;
		tmp.log_head_size = dev->share_para->log_head_size;
		pr_debug("[%s] write = %u, size = %d.\n", __func__,
			 tmp.log_write, tmp.log_head_size);
		if (copy_to_user((void __user *)args, &tmp, sizeof(tmp))) {
			pr_err("[%s] copy_to_user failed.\n", __func__);
			return -EFAULT;
        }
        break;
    default:
        pr_err("[%s] don't support cmd.\n", __func__);
        break;
    };

    pr_debug("[%s] -\n", __func__);

    return 0;
}

static int isplog_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct isplog_device_s *dev = (struct isplog_device_s *)&isplog_dev;
    u64 isprdr_addr;
    unsigned long size;
    int ret;

    pr_info("[%s] +\n", __func__);
    if (!dev->initialized) {
        pr_err("[%s] Failed : ISP RDR not ready\n", __func__);
        return -ENXIO;
    }

    if ((isprdr_addr = get_isprdr_addr()) == 0) {
        pr_err("[%s] Failed : isprdr_addr.0x%llx\n", __func__, isprdr_addr);
        return -ENOMEM;
    }

    if (vma->vm_start == 0) {
        pr_err("[%s] Failed : vm_start.0x%lx\n", __func__, vma->vm_start);
        return -EINVAL;
    }

    size = vma->vm_end - vma->vm_start;
    if (MEM_MAP_MAX_SIZE < size) {
        pr_err("%s: size.0x%lx.\n", __func__, size);
        return -EINVAL;
    }

    pr_info("[%s] enter, paddr = 0x%llx, vaddr.0x%lx, size.0x%lx.(0x%lx - 0x%lx), _prot0x%lx\n",
         __func__, isprdr_addr, vma->vm_start, size, vma->vm_end,
         vma->vm_start, (unsigned long)vma->vm_page_prot);
    vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
    ret = remap_pfn_range(vma, vma->vm_start,
                        (isprdr_addr >> PAGE_SHIFT),
                        size, vma->vm_page_prot);
    if (ret) {
        pr_err("[%s] remap_pfn_range failed, ret.%d\n", __func__, ret);
        return ret;
    }

    pr_info("[%s] -\n", __func__);

    return 0;
}

static int isplog_release(struct inode *inode, struct file *filp)
{
    struct isplog_device_s *dev = (struct isplog_device_s *)&isplog_dev;

    pr_info("[%s] +\n", __func__);
    if (!dev->initialized) {
        pr_err("[%s] Failed : ISP RDR not ready\n", __func__);
        return -ENXIO;
    }

	stop_isplogcat();
	dev->share_para = NULL;
	if (!use_sec_isp() && !use_nonsec_isp())
		iounmap(dev->share_mem);
	atomic_dec(&dev->open_cnt);
	pr_info("[%s] -\n", __func__);

    return 0;
}

static const struct file_operations isplog_ops = {
    .open           = isplog_open,
    .release        = isplog_release,
    .unlocked_ioctl = isplog_ioctl,
    .compat_ioctl   = isplog_ioctl,
    .mmap           = isplog_mmap,
    .owner          = THIS_MODULE,
};

static int __init isplog_init(void)
{
    struct isplog_device_s *dev = (struct isplog_device_s *)&isplog_dev;
    int ret = 0, major;

    pr_info("[%s] +\n", __func__);

    dev->initialized = 0;
    init_waitqueue_head(&dev->wait_ctl);
	ret = alloc_chrdev_region(&dev->log_cdev.devt, 0, 1, KBUILD_MODNAME);
	if (ret) {
		pr_err("%s: alloc_chrdev_region failed, ret.%d\n", __func__,
		       ret);
		goto out;
	}

	dev->log_cdev.log_dev_class = class_create(THIS_MODULE, KBUILD_MODNAME);
	if (IS_ERR(dev->log_cdev.log_dev_class)) {
		ret = PTR_ERR(dev->log_cdev.log_dev_class);
		pr_err("%s: class_create failed, ret.%d\n", __func__, ret);
		goto unreg_region;
	}

	major = MAJOR(dev->log_cdev.devt);
	pr_info("%s: log cdev major.%d.\n", __func__, major);

	cdev_init(&dev->log_cdev.log_chrdev, &isplog_ops);
	dev->log_cdev.log_chrdev.owner = THIS_MODULE;

	ret = cdev_add(&dev->log_cdev.log_chrdev, dev->log_cdev.devt, 1);
	if (ret) {
		pr_err("%s: cdev_add failed, ret.%d\n", __func__, ret);
		goto destroy_class;
	}

	dev->log_cdev.log_dev = device_create(dev->log_cdev.log_dev_class, NULL,
									dev->log_cdev.devt, NULL, "isp_log");
	if (IS_ERR(dev->log_cdev.log_dev)) {
		ret = PTR_ERR(dev->log_cdev.log_dev);
		pr_err("%s: device_create failed, ret.%d\n", __func__, ret);
		goto clean_cdev;
    }

    atomic_set(&dev->open_cnt, 0);
	atomic_set(&dev->timer_cnt, 0);
	setup_timer(&dev->sync_timer, sync_timer_fn, 0);
    dev->use_cacheable_rdr = 1;
    dev->initialized = 1;
    pr_info("[%s] -\n", __func__);

    return 0;
clean_cdev:
	cdev_del(&dev->log_cdev.log_chrdev);
destroy_class:
	class_destroy(dev->log_cdev.log_dev_class);
unreg_region:
	unregister_chrdev_region(dev->log_cdev.devt, 1);
out:
    pr_info("[%s] error -\n", __func__);

    return ret;
}

static void __exit isplog_exit(void)
{
    struct isplog_device_s *dev = (struct isplog_device_s *)&isplog_dev;

    pr_info("[%s] +\n", __func__);
    device_destroy(dev->log_cdev.log_dev_class, dev->log_cdev.devt);
    cdev_del(&dev->log_cdev.log_chrdev);
    class_destroy(dev->log_cdev.log_dev_class);
    unregister_chrdev_region(dev->log_cdev.devt, 1);
    dev->initialized = 0;
    pr_info("[%s] -\n", __func__);
}

module_init(isplog_init);
module_exit(isplog_exit);
MODULE_LICENSE("GPL v2");
