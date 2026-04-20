#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include "monitor_ioctl.h"

#define DEVICE "container_monitor"

struct node {
    pid_t pid;
    char id[32];
    struct node *next;
};

static struct node *head;
static int major;

static long dev_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {
        struct node *n = kmalloc(sizeof(*n), GFP_KERNEL);
        n->pid = req.pid;
        strcpy(n->id, req.container_id);
        n->next = head;
        head = n;

        printk(KERN_INFO "[monitor] REGISTER %s pid=%d\n", n->id, n->pid);
    }

    if (cmd == MONITOR_UNREGISTER) {
        struct node **cur = &head;
        while (*cur) {
            if ((*cur)->pid == req.pid) {
                struct node *tmp = *cur;
                *cur = (*cur)->next;
                kfree(tmp);
                printk(KERN_INFO "[monitor] UNREGISTER pid=%d\n", req.pid);
                break;
            }
            cur = &(*cur)->next;
        }
    }

    return 0;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = dev_ioctl,
};

static int __init init_mod(void)
{
    major = register_chrdev(0, DEVICE, &fops);
    printk(KERN_INFO "[monitor] loaded\n");
    return 0;
}

static void __exit exit_mod(void)
{
    unregister_chrdev(major, DEVICE);
    printk(KERN_INFO "[monitor] unloaded\n");
}

module_init(init_mod);
module_exit(exit_mod);

MODULE_LICENSE("GPL");
