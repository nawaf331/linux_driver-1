#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/semaphore.h>
#include <linux/proc_fs.h>

dev_t scull_major = 0, scull_minor = 0;
dev_t dev_no;

const int scull_nr_devs = 1;
const int scull_qset = 1000;
const int scull_quantum = 4000;

struct scull_qset
{
    void **data;
    struct scull_qset *next;
};

struct scull_dev
{
    struct scull_qset *data;
    int quantum;
    int qset;
    unsigned long size;
    unsigned int access_key;
    struct semaphore sem;
    struct cdev cdev;
} *dev;

static void scull_setup_cdev(struct scull_dev *dev, int index);
int scull_trim(struct scull_dev *dev);
int scull_open(struct inode *inode, struct file *filp);
int scull_release(struct inode *inode, struct file *filp);
ssize_t scull_write(struct file *filp, const char __user *buf, size_t count,
		    loff_t *f_pos);
ssize_t scull_read(struct file *filp, char __user *buf, size_t count,
		   loff_t *f_pos);
struct scull_qset* scull_follow(struct scull_dev *dev, int item);
int scull_read_procmem(char *buf, char **start, off_t offset, int count,
		       int *eof, void *data);

struct file_operations scull_fops = {
    .owner = THIS_MODULE,
    .read = scull_read,
    .write = scull_write,
    .open = scull_open,
    .release = scull_release,
};

static int __init scull_init(void)
{
    int i;
    dev = (struct scull_dev*)kmalloc(sizeof(struct scull_dev) * scull_nr_devs, GFP_KERNEL);
    for (i = 0; i < scull_nr_devs; i++)
    {
	scull_setup_cdev(&dev[i], i);
    }
    create_proc_read_entry("scullmem", 0, NULL, scull_read_procmem,
			   NULL);
    return 0;
}

static void __exit scull_cleanup(void)
{
    if (dev)
	kfree(dev);
    if (dev_no)
	unregister_chrdev_region(dev_no, scull_nr_devs);
}

module_init(scull_init);
module_exit(scull_cleanup);

static void scull_setup_cdev(struct scull_dev *dev, int index)
{
    int result = 0;
    int err;
    int devno = MKDEV(scull_major, scull_minor + index);

    if (scull_major)
    {
	dev_no = MKDEV(scull_major, scull_minor);
	result = register_chrdev_region(dev_no, scull_nr_devs, "scull");
    }
    else
    {
	result = alloc_chrdev_region(&dev_no, scull_minor, scull_nr_devs,
				     "scull");
	scull_major = MAJOR(dev_no);
    }

    cdev_init(&dev->cdev, &scull_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &scull_fops;
    err = cdev_add(&dev->cdev, devno, 1);

    if(err)
	printk(KERN_NOTICE "Error %d adding scull%d", err, index);
}

int scull_trim(struct scull_dev *dev)
{
    struct scull_qset *next, *dptr;
    int qset = dev->qset;
    int i;
    for (dptr = dev->data; dptr; dptr = next)
    {
	if (dptr->data)
	{
	    for (i = 0; i < qset; i++)
	    {
		kfree(dptr->data[i]);
	    }
	    kfree(dptr->data);
	    dptr->data = NULL;
	}
	next = dptr->next;
	kfree(dptr);
    }
    dev->size = 0;
    dev->quantum = scull_quantum;
    dev->qset = scull_qset;
    dev->data = NULL;
    return 0;
}

int scull_open(struct inode *inode, struct file *filp)
{
    struct scull_dev *dev;
    dev = container_of(inode->i_cdev, struct scull_dev, cdev);
    filp->private_data = dev;

    if ( (filp->f_flags & O_ACCMODE) == O_WRONLY)
    {
	scull_trim(dev);
    }

    return 0;
}

int scull_release(struct inode *inode, struct file *filp)
{
    remove_proc_entry("scullmem", NULL);
    return 0;
}

ssize_t scull_read(struct file *filp, char __user *buf, size_t count,
		   loff_t *f_pos)
{
    struct scull_dev *dev = filp->private_data;
    struct scull_qset *dptr;
    int quantum = dev->quantum, qset = dev->qset;
    int itemsize = quantum * qset;
    int item, s_pos, q_pos, rest;
    ssize_t retval = 0;

    if (down_interruptible(&dev->sem))
    {
	return -ERESTARTSYS;
    }
    if (*f_pos >= dev->size)
    {
	goto out;
    }
    if (*f_pos + count > dev->size)
    {
	count = dev->size - *f_pos;
    }

    item = (long)*f_pos / itemsize;
    rest = (long)*f_pos % itemsize;
    s_pos = rest / quantum;
    q_pos = rest % quantum;

    dptr = scull_follow(dev, item);

    if (dptr == NULL || !dptr->data || !dptr->data[s_pos])
    {
	goto out;
    }
    if (count > quantum - q_pos)
    {
	count = quantum - q_pos;
    }
    if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count))
    {
	retval = -EFAULT;
	goto out;
    }
    *f_pos += count;
    retval = count;

out:
    up(&dev->sem);
    return retval;
}

ssize_t scull_write(struct file *filp, const char __user *buf, size_t count,
		    loff_t *f_pos)
{
    struct scull_dev *dev = filp->private_data;
    struct scull_qset *dptr;
    int quantum = dev->quantum, qset = dev->qset;
    int itemsize = quantum * qset;
    int item, s_pos, q_pos, rest;
    ssize_t retval = -ENOMEM;

    if (down_interruptible(&dev->sem))
    {
	return -ERESTARTSYS;
    }

    item = (long) *f_pos / itemsize;
    rest = (long) *f_pos % itemsize;
    s_pos = rest / quantum;
    q_pos = rest % quantum;

    dptr = scull_follow(dev, item);
    if (dptr == NULL)
    {
	goto out;
    }
    if (!dptr->data)
    {
	dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL);
	if (!dptr->data)
	{
	    goto out;
	}
	memset(dptr->data, 0, qset * sizeof(char *));
    }
    if (!dptr->data[s_pos])
    {
	dptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL);
	if (!dptr->data[s_pos])
	{
	    goto out;
	}
    }

    if (count > quantum - q_pos)
    {
	count = quantum - q_pos;
    }
    if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count))
    {
	retval = -EFAULT;
	goto out;
    }
    *f_pos += count;
    retval = count;

    if (dev->size < *f_pos)
	dev->size = *f_pos;

out:
    up(&dev->sem);
    return retval;
}	

struct scull_qset* scull_follow(struct scull_dev *dev, int n)
{
    struct scull_qset *qs = dev->data;
    if (!qs)
    {
	qs = dev->data = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
	if (qs == NULL)
	{
	    return NULL;
	}
	memset(qs, 0, sizeof(struct scull_qset));
    }

    while (n--)
    {
	if (!qs->next)
	{
	    qs->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
	    if (qs->next == NULL)
		return NULL;
	    memset(qs->next, 0, sizeof(struct scull_qset));
	}
	qs = qs->next;
	continue;
    }
    return qs;
}

int scull_read_procmem(char *buf, char **start, off_t offset, int count,
		       int *eof, void *data)
{
    int i, j, len = 0;
    int limit = count - 80;

    for (i = 0; i < scull_nr_devs && len <= limit; i++)
    {
	struct scull_dev *d = &dev[i];
	struct scull_qset *qs = d->data;
	if (down_interruptible(&d->sem))
	    return -ERESTARTSYS;
	len += sprintf(buf + len, "\nDevice %i: qset %i, q %i, sz %li\n",
		       i, d->qset, d->quantum, d->size);
	for (; qs && len <= limit; qs = qs->next)
	{
	    len += sprintf(buf + len, "  item at %p, qset at %p\n",
			   qs, qs->data);
	    if (qs->data && !qs->next)
	    {
		for (j = 0; j < d->qset; j++)
		{
		    if (qs->data[j])
		    {
			len += sprintf(buf + len, "  %4i: %8p\n", j,
				       qs->data[j]);
		    }
		}
	    }
	}
	up(&dev[i].sem);
    }
    *eof = 1;
    return len;
}
