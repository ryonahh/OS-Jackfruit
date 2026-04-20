#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"

struct entry {
    pid_t pid;
    unsigned long soft;
    unsigned long hard;
    char id[32];
    struct entry *next;
};

static struct entry *head = NULL;
static int major;

static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {
        struct entry *e = kmalloc(sizeof(*e), GFP_KERNEL);
        if (!e) return -ENOMEM;

        e->pid = req.pid;
        e->soft = req.soft_limit_bytes;
        e->hard = req.hard_limit_bytes;
        strncpy(e->id, req.container_id, 31);
        e->id[31] = '\0';

        e->next = head;
        head = e;

        printk(KERN_INFO "[monitor] REGISTER pid=%d id=%s\n", e->pid, e->id);
    }

    else if (cmd == MONITOR_UNREGISTER) {
        struct entry **curr = &head;

        while (*curr) {
            if ((*curr)->pid == req.pid) {
                struct entry *tmp = *curr;
                *curr = (*curr)->next;
                kfree(tmp);
                printk(KERN_INFO "[monitor] UNREGISTER pid=%d\n", req.pid);
                break;
            }
            curr = &(*curr)->next;
        }
    }

    return 0;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = device_ioctl,
};

static int __init monitor_init(void)
{
    major = register_chrdev(0, DEVICE_NAME, &fops);
    printk(KERN_INFO "[monitor] loaded, major=%d\n", major);
    return 0;
}

static void __exit monitor_exit(void)
{
    unregister_chrdev(major, DEVICE_NAME);
    printk(KERN_INFO "[monitor] unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
