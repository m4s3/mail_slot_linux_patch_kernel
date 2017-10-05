/*
 *  mail slot  dev file driver @1.0 - you can put or retrieve streams of bytes in a fifo mode into a mail slot
 *  preliminary you need to mknod the dev file and assign the major retrived while mounting this module
 */


#define EXPORT_SYMTAB

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/pid.h>



MODULE_LICENSE("GPL");
MODULE_AUTHOR("Massimiliano Cestra");
MODULE_DESCRIPTION("this module implements a device file driver for linux  fifo mail slot");

#define MODNAME "FIFO_MAIL_SLOT"
#define DEVICE_NAME "mslot"  /* Device file name in /dev/ - not mandatory  */

#define MAX_MAIL_SLOT_SIZE (1<<20) //1MB of max storage ///UPPER LIMIT
#define MAX_SEGMENT_SIZE (1<<10) //1KB of max segment size ///UPPER LIMIT
#define MAX_MINOR_NUM (256)

#define DEBUG if(1)
#define NO (0)
#define YES (NO+1)

#define CURRENT_DEVICE MINOR(filp->f_dentry->d_inode->i_rdev)

// TUNABLE

#define ACTUAL_BLOCKING_MODE_CTL 6
#define ACTUAL_MAXIMUM_SEGMENT_SIZE_CTL 3

//ACTUAL_BLOCKING_CTL options

#define BLOCKING_MODE 0
#define NON_BLOCKING_MODE 1

//GET

#define GET_MAX_SEGMENT_SIZE 7
#define GET_FREE_SIZE 8

typedef struct segment{
	int size;
	struct segment* next;
	char* payload;
} segment;

typedef struct _elem{
	struct task_struct *task;
	int pid;
	int awake;
    int len;
	int already_hit;
	struct _elem * next;
	struct _elem * prev;
} elem;

typedef struct _list{
    elem head;
    elem tail;
} list;

static int ms_open(struct inode *, struct file *);
static int ms_release(struct inode *, struct file *);
static ssize_t ms_write(struct file *, const char *, size_t, loff_t *);
static ssize_t ms_read(struct file * , char * , size_t , loff_t * );
static long ms_ctl (struct file *filp, unsigned int param1, unsigned long param2);
static void print_q(elem *,elem *);
//static void awake_until(int , elem * , elem *);

static elem my_head = {NULL,-1,-1,-1,-1,NULL,NULL};
static elem my_tail = {NULL,-1,-1,-1,-1,NULL,NULL};
static int Major;
static segment * head[MAX_MINOR_NUM];
static segment * tail[MAX_MINOR_NUM];
static list  list_r[MAX_MINOR_NUM];
static list  list_w[MAX_MINOR_NUM];
static spinlock_t lock[MAX_MINOR_NUM];
static int used[MAX_MINOR_NUM];
static int actual_maximum_segment_size[MAX_MINOR_NUM];
static int blk_mode[MAX_MINOR_NUM];

/* the actual driver */

//IOCTL
static long ms_ctl(struct file *filp, unsigned int cmd, unsigned long arg){
    int current_device,to_ret,old_maximum_size;

	DEBUG
        printk("%s : ioctl. cmd is %d arg is %ld\n",MODNAME,cmd,arg);

    current_device=CURRENT_DEVICE;

	if(blk_mode[current_device]==NON_BLOCKING_MODE && (spin_trylock(&lock[current_device]))==0){
		DEBUG
            printk("%s : some process is using the device file  with minor %d   and the process %d is not blocking\n",MODNAME,current_device,current->pid);
		return -1;
    }

	else if (blk_mode[current_device]==BLOCKING_MODE)
		spin_lock(&lock[CURRENT_DEVICE]);

	switch(cmd){

		case ACTUAL_BLOCKING_MODE_CTL: //BLOCKING OR NON BLOCKING
            DEBUG
                printk("%s: modifying the block/noblock configuration\n",MODNAME);

            blk_mode[current_device] = arg;
            spin_unlock(&lock[current_device]);
			break;

		case ACTUAL_MAXIMUM_SEGMENT_SIZE_CTL:
            DEBUG
                printk("%s: modifying the maximum segment size\n",MODNAME);

			if(arg<=MAX_SEGMENT_SIZE){

				actual_maximum_segment_size[current_device] = arg;

                spin_unlock(&lock[current_device]);
                DEBUG
                    printk("%s : ACTUAL_MAXIMUM_SEGMENT_SIZE is %d\n",MODNAME,actual_maximum_segment_size[current_device]);
            }
			else{
				spin_unlock(&lock[current_device]);
				return -EINVAL;
			}
			break;

		case GET_MAX_SEGMENT_SIZE:
            DEBUG
                printk("%s: getting the max segment size\n",MODNAME);

            to_ret = actual_maximum_segment_size[current_device];
			spin_unlock(&lock[current_device]);
			return to_ret;

		case GET_FREE_SIZE:
            DEBUG
                printk("%s: getting the free size\n",MODNAME);

            to_ret = MAX_MAIL_SLOT_SIZE - used[current_device];
			spin_unlock(&lock[current_device]);
			return to_ret;

		default:
			printk("%s : DEFAULT\n",MODNAME);
			spin_unlock(&lock[current_device]);
			return -ENOTTY;
	}

	return 0;
}

static int ms_open(struct inode *inode, struct file *filp){

    int tmp_minor;
    tmp_minor=CURRENT_DEVICE;

    DEBUG
        printk("%s: somebody called an open  on mail slot  dev with minor number %d\n",MODNAME,tmp_minor);

    if (tmp_minor >= MAX_MINOR_NUM || tmp_minor < 0 ){
        DEBUG
            printk("%s: error => somebody called an open  on mail slot  dev with minor number %d\n",MODNAME,tmp_minor);
        return -1;
    }
    try_module_get(THIS_MODULE);
    return 0;
}


static int ms_release(struct inode *inode, struct file *filp)
{
    DEBUG
        printk("%s: somebody called a close  on mail slot  dev with minor number %d\n",MODNAME,CURRENT_DEVICE);
    module_put(THIS_MODULE);
    return 0;

}

static ssize_t ms_write(struct file *filp,const char *buff,size_t len,loff_t *off){
    int calling_device,res;
    segment * new;
    char * tmp;
    volatile elem me;
	elem *aux;
	DECLARE_WAIT_QUEUE_HEAD(the_queue);

	me.next = NULL;
	me.prev = NULL;
	me.task = current;
	me.pid  = current->pid;
	me.awake = NO;
    me.len=len;
	me.already_hit = NO;

    DEBUG
        printk("%s: somebody called a write  on mail slot  dev with minor number %d\n",MODNAME,CURRENT_DEVICE);

    if(len > MAX_SEGMENT_SIZE || len <=0 ){

        DEBUG
            printk("%s : Message cancelled. len=%zu, max segment size =%d \n",MODNAME,len,MAX_SEGMENT_SIZE);
        return -EMSGSIZE;

    }
    calling_device = CURRENT_DEVICE;

    //i'm preallocating memory here because I might go to sleep but i'm not in critical section
    tmp=kmalloc(len,GFP_KERNEL);
    memset(tmp,0,len);
    copy_from_user(tmp,buff,len);

    new = kmalloc(sizeof(segment),GFP_KERNEL);
	memset(new,0,sizeof(segment));
	new->payload = kmalloc(len,GFP_KERNEL);
	memset(new->payload,0,len);

    spin_lock(&lock[calling_device]);

     //this check is done because during the time the ioctl could change the actual size of the segments

    if(len > actual_maximum_segment_size[calling_device]){

        printk("%s : Message cancelled. len=%zu, actual_maximum_size=%d \n",MODNAME,len, actual_maximum_segment_size[calling_device]);
        spin_unlock(&lock[calling_device]);

        //the operations of free are done out by critical section
        kfree(tmp);
        kfree(new->payload);
        kfree(new);
        return -EMSGSIZE;
    }

    //the follow lines must be done in critical section...size of  segment could change

    //full, || the message is too large for the mail slot actually

    while((MAX_MAIL_SLOT_SIZE -used[calling_device]) = 0 || len>( MAX_MAIL_SLOT_SIZE -used[calling_device] ){



        if(blk_mode[calling_device]==NON_BLOCKING_MODE){
            spin_unlock(&lock[calling_device]);
            kfree(tmp);
            kfree(new->payload);
            kfree(new);
            return -EAGAIN;
        }

        //the writer has to put itself in the list's tail
        aux = &(list_w[calling_device].tail);

        if(aux->prev == NULL){
            spin_unlock(&lock[calling_device]);
            printk("%s: malformed sleep-list - service damaged\n",MODNAME);
            kfree(tmp);
            kfree(new->payload);
            kfree(new);
            return -1;
        }

        aux->prev->next = &me;
        me.prev = aux->prev;
        aux->prev = &me;
        me.next = aux;

        DEBUG
            print_q(&(list_w[calling_device].head),&(list_w[calling_device].tail));

        spin_unlock(&lock[calling_device]);
        res = wait_event_interruptible(the_queue, len <= (MAX_MAIL_SLOT_SIZE - used[calling_device] ) );

        if(res!=0){
            printk("%s:the process with pid %d has been awaked by a signal\n",MODNAME,current->pid);
            kfree(tmp);
            kfree(new->payload);
            kfree(new);
            return -ERESTARTSYS;
        }
        else
            printk("%s: res is %d\n",MODNAME,res);

        //the writer has to delete itself in the list
        spin_lock(&lock[calling_device]);
        aux = &(list_w[calling_device].head);

        if(aux == NULL){
            spin_unlock(&lock[calling_device]);
            printk("%s: malformed sleep-list upon wakeup - service damaged\n",MODNAME);
            kfree(tmp);
            kfree(new->payload);
            kfree(new);
            return -1;
        }

        me.prev->next = me.next;//we know where we are thanks to double linkage
        me.next->prev = me.prev;

         //this check is done because during the time the ioctl could change the actual size of the segments

        if(len > actual_maximum_segment_size[calling_device]){

            printk("%s : Message cancelled. len=%zu, actual_maximum_size=%d \n",MODNAME,len, actual_maximum_segment_size[calling_device]);
            spin_unlock(&lock[calling_device]);

            //the operations of free are done out by critical section
            kfree(tmp);
            kfree(new->payload);
            kfree(new);
            return -EMSGSIZE;
        }

    }

    memcpy(new->payload,tmp,len);  //here there is only kernel memory
	new->size = len;
	new->next = NULL;
    //now I have to add a new segment in the mailslot's tail

	if(head[calling_device]==NULL){
		head[calling_device] = new;
		tail[calling_device] = new;
	}
	else{
		tail[calling_device]->next = new;
		tail[calling_device] = new;
	}

	used[calling_device] += len;

    //now I have to awake a reader
    aux = &(list_r[calling_device].head);

	if(aux == NULL){

		spin_unlock(&lock[calling_device]);
		printk("%s: malformed sleep-wakeup-queue\n",MODNAME);
        kfree(tmp);
		return -1;
	}

    while(aux->next != &(list_r[calling_device].tail)){

		if(aux->next->already_hit == NO){
			aux->next->awake = YES;
			aux->next->already_hit = YES;
			wake_up_process(aux->next->task);
			break;
		}

		aux = aux->next;

	}
    spin_unlock(&lock[calling_device]);
    kfree(tmp);


   return len;
}



static ssize_t ms_read(struct file * filp , char * buff , size_t  len , loff_t * off){
    int current_device,res;
    segment * old_head;
    char * ker_buf;
    current_device=CURRENT_DEVICE;
    volatile elem me;
	elem *aux;
	DECLARE_WAIT_QUEUE_HEAD(the_queue);

	me.next = NULL;
	me.prev = NULL;
	me.task = current;
	me.pid  = current->pid;
	me.awake = NO;
	me.already_hit = NO;

    DEBUG
        printk("%s: somebody called a read  on mail slot  dev with minor number %d\n",MODNAME,current_device);
    if ( len <= 0 )
        return -EMSGSIZE;

    if(len > MAX_SEGMENT_SIZE)
        len=MAX_SEGMENT_SIZE;

    //i'm preparing a temp buffer in order to move the data in a critical section without the risk to go in sleep mode
    ker_buf = kmalloc(len,GFP_KERNEL);
    memset(ker_buf,0,len);
    spin_lock(&lock[current_device]);

    while(head[current_device]==NULL){
        if (blk_mode[current_device] == NON_BLOCKING_MODE){
            spin_unlock(&lock[current_device]);
            kfree(ker_buf);
            return -EAGAIN;
        }
        else{

            //the reader has to put in the list's tail
            aux = &(list_r[current_device].tail);

            if(aux->prev == NULL){
                spin_unlock(&lock[current_device]);
                printk("%s: malformed sleep-list - service damaged\n",MODNAME);
                kfree(ker_buf);
                return -1;
            }

            aux->prev->next = &me;
            me.prev = aux->prev;
            aux->prev = &me;
            me.next = aux;

            DEBUG
                print_q(&(list_r[current_device].head),&(list_r[current_device].tail));

            spin_unlock(&lock[current_device]);
            DEBUG
                printk("%s : the process with pid = %d is going to sleep\n",MODNAME,current->pid);

            res = wait_event_interruptible(the_queue,head[current_device]!=NULL);

            if(res!=0){
                printk("%s:the process with pid %d has been awaked by a signal\n",MODNAME,current->pid);
                kfree(ker_buf);
                return -ERESTARTSYS;
            }

            printk("%s: res is %d\n",MODNAME,res);
            spin_lock(&lock[current_device]);
            //the reader has to delete in the list
            aux = &(list_r[current_device].head);

            if(aux == NULL){
                spin_unlock(&lock[current_device]);
                printk("%s: malformed sleep-list upon wakeup - service damaged\n",MODNAME);
                kfree(ker_buf);
                return -1;
            }

            me.prev->next = me.next;//we know where we are thanks to double linkage
            me.next->prev = me.prev;

            DEBUG
                print_q(&(list_r[current_device].head),&(list_r[current_device].tail));

        }
    }
    printk("%s : len = %du and size = %du\n",MODNAME,len,head[current_device]->size);

    if(len <  head[current_device]->size){
        DEBUG
            printk("%s: someone is trying to read an amount of data less than head segment size \n",MODNAME);

        spin_unlock(&lock[current_device]);
        kfree(ker_buf);
        return -EINVAL;

    }

    else{
        len=head[current_device]->size;
        old_head = head[current_device];
        memcpy(ker_buf, head[current_device]->payload,len);
        used[current_device]-=len;

        if(tail[current_device] == head[current_device])
            tail[current_device] = NULL;
        head[current_device] = head[current_device]->next;

        //the reader has to awake a writer in the list
        aux = &(list_w[current_device].head);

        if(aux == NULL){
            spin_unlock(&lock[current_device]);
            printk("%s: malformed sleep-list\n",MODNAME);
            kfree(ker_buf);
            return -1;
        }

        while(aux->next != &(list_w[current_device].tail)){

            if(aux->next->already_hit == NO){
                aux->next->awake = YES;
                aux->next->already_hit = YES;
                wake_up_process(aux->next->task);
                break;
            }

            aux = aux->next;

        }

        spin_unlock(&lock[current_device]);

        copy_to_user(buff,ker_buf,len); //here i might go to sleep but i'm not in critical section
        kfree(old_head->payload);
        kfree(old_head);
        kfree(ker_buf);


    }

    return len;

}




static void print_q(elem * head,elem * tail){
    elem * aux = head;

    while(aux->next != tail){
        printk("%s: there is a process with pid = %d\n",MODNAME,aux->next->task->pid);
        aux=aux->next;
    }
}





static struct file_operations fops = {
  .owner = THIS_MODULE,
  .write = ms_write,
  .open =  ms_open,
  .release = ms_release,
  .read = ms_read,
  .unlocked_ioctl = ms_ctl
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
        blk_mode[i]=BLOCKING_MODE;
        spin_lock_init(&lock[i]);
        list_r[i].head=my_head;
        list_r[i].tail=my_tail;
        list_r[i].head.next=&list_r[i].tail;
        list_r[i].tail.prev=&list_r[i].head;
        list_w[i].head=my_head;
        list_w[i].tail=my_tail;
        list_w[i].head.next=&list_w[i].tail;
        list_w[i].tail.prev=&list_w[i].head;
        //better open
    }

	return 0;
}

void cleanup_module(void)
{

    int i;
    for(i=0;i<MAX_MINOR_NUM;i++){
        while(head[i]!=NULL){
            segment* to_die=head[i];
            head[i]=head[i]->next;
            kfree(to_die->payload);
            kfree(to_die);
			}
	}
	unregister_chrdev(Major, DEVICE_NAME);

	printk(KERN_INFO "Mail slot  device unregistered, it was assigned major number %d\n", Major);
}
