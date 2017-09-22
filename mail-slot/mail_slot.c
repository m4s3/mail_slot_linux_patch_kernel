
/*
 *  mail slot  dev file driver - you can put or retrieve streams of bytes into a mail slot
 *  preliminary you need to mknod the dev file and assign the major retrived while mounting this module
 */

#define EXPORT_SYMTAB
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/sched.h>
#include <linux/pid.h>/* For pid types */
#include <linux/version.h>/* For LINUX_VERSION_CODE */


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Massimiliano Cestra");
MODULE_DESCRIPTION("this module implements a device file driver for linux  fifo mail slot");

#define MODNAME "FIFO MAIL SLOT"
#define DEVICE_NAME "mslot"  /* Device file name in /dev/ - not mandatory  */

#define CURRENT_DEVICE MINOR(filp->f_dentry->d_inode->i_rdev)
#define MAX_MAIL_SLOT_SIZE (1<<20) //1MB of max storage ///UPPER LIMIT
#define MAX_SEGMENT_SIZE (1<<10) //1KB of max segment size ///UPPER LIMIT
#define MAX_MINOR_NUM (256)
#define BLOCKING_MODE (0)
#define NON_BLOCKING_MODE (1)

#define DEBUG if(1)

atomic_t count ;//this is used to audit how many session  are still open
module_param(count,long,S_IRUGO|S_IWUSR);


static int ms_open(struct inode *, struct file *);
static int ms_release(struct inode *, struct file *);
static ssize_t ms_write(struct file *, const char *, size_t, loff_t *);
static ssize_t ms_read(struct file * , char * , size_t , loff_t * );

typedef struct segment{
	int size;
	struct segment* next;
	char* payload;
} segment;

static int Major; /* Major number assigned to mail slot device driver */
static segment * head[MAX_MINOR_NUM];
static segment * tail[MAX_MINOR_NUM];
static spinlock_t lock[MAX_MINOR_NUM];
static wait_queue_head_t wait_queues[MAX_MINOR_NUM];
static int used[MAX_MINOR_NUM];
static int actual_maximum_size[MAX_MINOR_NUM];
static int actual_maximum_segment_size[MAX_MINOR_NUM];
static int blk_mode[MAX_MINOR_NUM];


/* auxiliary stuff */
// will i do anything?


/* the actual driver */


static int ms_open(struct inode *inode, struct file *filp){

    atomic_inc(&count);//a new session
    int tmp_minor=CURRENT_DEVICE;
    DEBUG
        printk("%s: somebody called an open  on mail slot  dev with minor number %d\n",MODNAME,tmp_minor);


    if (tmp_minor >= MAX_MINOR_NUM || tmp_minor < 0 ){ //check on device's minor number
        DEBUG
            printk("%s: error: somebody called an open  on mail slot  dev with minor number %d\n",MODNAME,tmp_minor);
        atomic_dec(&count);
        return -1;
    }

   return 0;
}


static int ms_release(struct inode *inode, struct file *filp)
{
    atomic_dec(&count);
    DEBUG
        printk("%s: somebody called a close  on mail slot  dev with minor number %d\n",MODNAME,CURRENT_DEVICE);
    //device closed by default nop
   return 0;

}




 // #define LINE_SIZE 128

static ssize_t ms_write(struct file *filp,const char *buff,size_t len,loff_t *off){
    DEBUG
        printk("%s: somebody called a write  on mail slot  dev with minor number %d\n",MODNAME,CURRENT_DEVICE);

   return len;
}


static ssize_t ms_read(struct file * filp , char * buff , size_t  len , loff_t * off){
    DEBUG
        printk("%s: somebody called a read  on mail slot  dev with minor number %d\n",MODNAME,CURRENT_DEVICE);
    return 0;
}



static struct file_operations fops = {
  .write = ms_write,
  .open =  ms_open,
  .release = ms_release,
  .read = ms_read
};



int init_module(void)
{
    int i;
	Major = register_chrdev(0, DEVICE_NAME, &fops);

	if (Major < 0) {
	  printk("Registering mail slot device failed\n");
	  return Major;
	}
    DEBUG
        printk(KERN_INFO "Mail slot device registered, it is assigned major number %d\n", Major);

    for(i=0;i<MAX_MINOR_NUM;i++){
        actual_maximum_segment_size[i]=MAX_SEGMENT_SIZE;
        actual_maximum_size[i]=MAX_MAIL_SLOT_SIZE;
        blk_mode[i]=BLOCKING_MODE;
        spin_lock_init(&lock[i]);
        init_waitqueue_head(&wait_queues[i]);
    }

	return 0;
}

void cleanup_module(void)
{

	unregister_chrdev(Major, DEVICE_NAME);

	printk(KERN_INFO "Mail slot  device unregistered, it was assigned major number %d\n", Major);
}
