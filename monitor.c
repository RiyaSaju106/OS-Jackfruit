#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>
#include <linux/signal.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CLASS_NAME  "container_monitor_class"
#define MONITOR_INTERVAL_MS 1000

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS Mini Project");
MODULE_DESCRIPTION("Kernel memory monitor for container processes");
MODULE_VERSION("1.0");

/* ================= TRACKED ENTRY ================= */
struct monitor_entry {
    pid_t pid;
    unsigned long soft_limit;   /* MiB */
    unsigned long hard_limit;   /* MiB */
    int soft_warned;
    struct list_head list;
};

/* ================= GLOBAL STATE ================= */
static dev_t monitor_dev;
static struct cdev monitor_cdev;

static struct class *monitor_class;
static struct device *monitor_device;

static LIST_HEAD(monitor_list);
static DEFINE_MUTEX(monitor_lock);

static struct task_struct *monitor_thread;

/* ================= CLASS CREATE COMPAT ================= */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define MONITOR_CLASS_CREATE(name) class_create(name)
#else
#define MONITOR_CLASS_CREATE(name) class_create(THIS_MODULE, name)
#endif

/* ================= HELPERS ================= */
static struct monitor_entry *find_entry_locked(pid_t pid)
{
    struct monitor_entry *entry;

    list_for_each_entry(entry, &monitor_list, list) {
        if (entry->pid == pid)
            return entry;
    }

    return NULL;
}

static int remove_entry_locked(pid_t pid)
{
    struct monitor_entry *entry, *tmp;

    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
        if (entry->pid == pid) {
            list_del(&entry->list);
            kfree(entry);
            return 0;
        }
    }

    return -ENOENT;
}

static int process_exists(pid_t pid)
{
    struct pid *kpid;
    struct task_struct *task;
    int exists = 0;

    kpid = find_get_pid(pid);
    if (!kpid)
        return 0;

    task = pid_task(kpid, PIDTYPE_PID);
    if (task)
        exists = 1;

    put_pid(kpid);
    return exists;
}

static int get_process_rss_mib(pid_t pid, unsigned long *rss_mib_out)
{
    struct pid *kpid;
    struct task_struct *task;
    struct mm_struct *mm;
    unsigned long rss_pages;
    unsigned long rss_bytes;
    unsigned long rss_mib;

    if (!rss_mib_out)
        return -EINVAL;

    *rss_mib_out = 0;

    kpid = find_get_pid(pid);
    if (!kpid)
        return -ESRCH;

    task = pid_task(kpid, PIDTYPE_PID);
    if (!task) {
        put_pid(kpid);
        return -ESRCH;
    }

    mm = get_task_mm(task);
    if (!mm) {
        put_pid(kpid);
        return -ESRCH;
    }

    rss_pages = get_mm_rss(mm);
    rss_bytes = rss_pages * PAGE_SIZE;
    rss_mib = rss_bytes / (1024 * 1024);

    mmput(mm);
    put_pid(kpid);

    *rss_mib_out = rss_mib;
    return 0;
}

static int kill_process_by_pid(pid_t pid)
{
    struct pid *kpid;
    int ret;

    kpid = find_get_pid(pid);
    if (!kpid)
        return -ESRCH;

    ret = kill_pid(kpid, SIGKILL, 1);
    put_pid(kpid);

    return ret;
}

/* ================= IOCTL HANDLER ================= */
static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    monitor_request_t req;
    pid_t pid;
    struct monitor_entry *entry;

    switch (cmd) {
    case REGISTER_CONTAINER:
        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;

        if (req.pid <= 0)
            return -EINVAL;

        if (req.hard_limit == 0)
            return -EINVAL;

        mutex_lock(&monitor_lock);

        entry = find_entry_locked(req.pid);
        if (entry) {
            /* Update existing entry instead of failing */
            entry->soft_limit = req.soft_limit;
            entry->hard_limit = req.hard_limit;
            entry->soft_warned = 0;

            mutex_unlock(&monitor_lock);

            pr_info("monitor: updated PID=%d soft=%lu MiB hard=%lu MiB\n",
                    req.pid, req.soft_limit, req.hard_limit);
            return 0;
        }

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry) {
            mutex_unlock(&monitor_lock);
            return -ENOMEM;
        }

        entry->pid = req.pid;
        entry->soft_limit = req.soft_limit;
        entry->hard_limit = req.hard_limit;
        entry->soft_warned = 0;
        INIT_LIST_HEAD(&entry->list);

        list_add_tail(&entry->list, &monitor_list);
        mutex_unlock(&monitor_lock);

        pr_info("monitor: registered PID=%d soft=%lu MiB hard=%lu MiB\n",
                req.pid, req.soft_limit, req.hard_limit);
        return 0;

    case UNREGISTER_CONTAINER:
        if (copy_from_user(&pid, (void __user *)arg, sizeof(pid)))
            return -EFAULT;

        mutex_lock(&monitor_lock);
        if (remove_entry_locked(pid) == 0) {
            mutex_unlock(&monitor_lock);
            pr_info("monitor: unregistered PID=%d\n", pid);
            return 0;
        }
        mutex_unlock(&monitor_lock);

        return -ENOENT;

    default:
        return -EINVAL;
    }
}

/* ================= FILE OPS ================= */
static int monitor_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int monitor_release(struct inode *inode, struct file *file)
{
    return 0;
}

static const struct file_operations monitor_fops = {
    .owner = THIS_MODULE,
    .open = monitor_open,
    .release = monitor_release,
    .unlocked_ioctl = monitor_ioctl,
};

/* ================= MONITOR THREAD ================= */
static int monitor_thread_fn(void *data)
{
    struct monitor_entry *entry, *tmp;

    while (!kthread_should_stop()) {
        mutex_lock(&monitor_lock);

        list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
            unsigned long rss_mib = 0;
            int ret;

            if (!process_exists(entry->pid)) {
                pr_info("monitor: removing stale PID=%d\n", entry->pid);
                list_del(&entry->list);
                kfree(entry);
                continue;
            }

            ret = get_process_rss_mib(entry->pid, &rss_mib);
            if (ret != 0) {
                pr_info("monitor: failed RSS lookup for PID=%d, removing entry\n",
                        entry->pid);
                list_del(&entry->list);
                kfree(entry);
                continue;
            }

            if (!entry->soft_warned && rss_mib > entry->soft_limit) {
                pr_warn("monitor: soft limit exceeded PID=%d RSS=%lu MiB soft=%lu MiB\n",
                        entry->pid, rss_mib, entry->soft_limit);
                entry->soft_warned = 1;
            }

            if (rss_mib > entry->hard_limit) {
                pr_err("monitor: hard limit exceeded PID=%d RSS=%lu MiB hard=%lu MiB -> killing\n",
                       entry->pid, rss_mib, entry->hard_limit);

                kill_process_by_pid(entry->pid);

                /*
                 * Keep entry until user-space unregisters or process disappears.
                 * This helps the engine see the final signal-based exit first.
                 */
            }
        }

        mutex_unlock(&monitor_lock);
        msleep(MONITOR_INTERVAL_MS);
    }

    return 0;
}

/* ================= MODULE INIT ================= */
static int __init monitor_init(void)
{
    int ret;

    ret = alloc_chrdev_region(&monitor_dev, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("monitor: alloc_chrdev_region failed\n");
        return ret;
    }

    cdev_init(&monitor_cdev, &monitor_fops);
    monitor_cdev.owner = THIS_MODULE;

    ret = cdev_add(&monitor_cdev, monitor_dev, 1);
    if (ret < 0) {
        pr_err("monitor: cdev_add failed\n");
        unregister_chrdev_region(monitor_dev, 1);
        return ret;
    }

    monitor_class = MONITOR_CLASS_CREATE(CLASS_NAME);
    if (IS_ERR(monitor_class)) {
        pr_err("monitor: class_create failed\n");
        cdev_del(&monitor_cdev);
        unregister_chrdev_region(monitor_dev, 1);
        return PTR_ERR(monitor_class);
    }

    monitor_device = device_create(monitor_class, NULL, monitor_dev, NULL, DEVICE_NAME);
    if (IS_ERR(monitor_device)) {
        pr_err("monitor: device_create failed\n");
        class_destroy(monitor_class);
        cdev_del(&monitor_cdev);
        unregister_chrdev_region(monitor_dev, 1);
        return PTR_ERR(monitor_device);
    }

    monitor_thread = kthread_run(monitor_thread_fn, NULL, "container_monitor_thread");
    if (IS_ERR(monitor_thread)) {
        pr_err("monitor: failed to create monitor thread\n");
        device_destroy(monitor_class, monitor_dev);
        class_destroy(monitor_class);
        cdev_del(&monitor_cdev);
        unregister_chrdev_region(monitor_dev, 1);
        return PTR_ERR(monitor_thread);
    }

    pr_info("monitor: loaded successfully\n");
    pr_info("monitor: device created at /dev/%s\n", DEVICE_NAME);
    return 0;
}

/* ================= MODULE EXIT ================= */
static void __exit monitor_exit(void)
{
    struct monitor_entry *entry, *tmp;

    if (monitor_thread)
        kthread_stop(monitor_thread);

    mutex_lock(&monitor_lock);
    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&monitor_lock);

    device_destroy(monitor_class, monitor_dev);
    class_destroy(monitor_class);
    cdev_del(&monitor_cdev);
    unregister_chrdev_region(monitor_dev, 1);

    pr_info("monitor: unloaded successfully\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
