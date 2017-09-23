
/*
 *  mail slot  dev file driver - you can put or retrieve streams of bytes into a mail slot
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
#include <linux/slab.h>/* For kmalloc */
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
#define DEBUG if(1)

//WHAT IS TUNABLE
#define ACTUAL_MAXIMUM_SIZE_CTL 0
#define ACTUAL_BLOCKING_MODE_CTL 6
#define ACTUAL_MAXIMUM_SEGMENT_SIZE_CTL 3
#define PURGE_OPTION_CTL 9
//ACTUAL_BLOCKING_CTL options
#define BLOCKING_MODE 0
#define NON_BLOCKING_MODE 1

//PURGE PURGE_OPTION_CTL options
#define PURGE 1
#define NO_PURGE 0
//GET
#define GET_MAX_SEGMENT_SIZE 7
#define GET_FREE_SIZE 8






static int ms_open(struct inode *, struct file *);
static int ms_release(struct inode *, struct file *);
static ssize_t ms_write(struct file *, const char *, size_t, loff_t *);
static ssize_t ms_read(struct file * , char * , size_t , loff_t * );
static long ms_ctl (struct file *filp, unsigned int param1, unsigned long param2);
static void purge(int current_device);

typedef struct segment{
	int size;
	struct segment* next;
	char* payload;
} segment;

atomic_t count ;//this is used to audit how many session  are still open
module_param(count,long,S_IRUGO|S_IWUSR);


static int Major; /* Major number assigned to mail slot device driver */
static segment * head[MAX_MINOR_NUM];
static segment * tail[MAX_MINOR_NUM];
static spinlock_t lock[MAX_MINOR_NUM];
static wait_queue_head_t wait_queues[MAX_MINOR_NUM];
static int used[MAX_MINOR_NUM];
static int actual_maximum_size[MAX_MINOR_NUM];
static int actual_maximum_segment_size[MAX_MINOR_NUM];
static int blk_mode[MAX_MINOR_NUM];
static int actual_purge_option[MAX_MINOR_NUM];


/* auxiliary stuff */
// will i do anything?


/* the actual driver */

//IOCTL
static long ms_ctl(struct file *filp, unsigned int cmd, unsigned long arg){
    int current_device,to_ret;
	DEBUG
        printk("%s : ioctl. cmd is %d arg is %ld\n",MODNAME,cmd,arg);

    current_device=CURRENT_DEVICE;



	if(blk_mode[current_device]==NON_BLOCKING_MODE && (spin_trylock(&lock[current_device]))==0){
		DEBUG
            printk("%s : some process is using the device file  with minor %d   and it is in non blocking mode\n",MODNAME,current_device);
		return -1;
    }

	else if (blk_mode[current_device]==BLOCKING_MODE)
		spin_lock(&lock[CURRENT_DEVICE]);

	//se sono qui è perchè o sono asincrono o perchè sono bloccante  e ho passato il lock
	switch(cmd){

		case ACTUAL_BLOCKING_MODE_CTL: //BLOCKING OR NON BLOCKING
            DEBUG
                printk("%s: modifying the block/noblock configuration\n",MODNAME);
            blk_mode[current_device] = arg;
            spin_unlock(&lock[current_device]);
			break;

		case ACTUAL_MAXIMUM_SIZE_CTL:
            DEBUG
                printk("%s: modifying the maximum size\n",MODNAME);
			if(arg > 0 && arg<=MAX_MAIL_SLOT_SIZE){ ///absolute upper limit
				actual_maximum_size[current_device] = arg;
                if(actual_purge_option[current_device]==PURGE){
					//vedere se funziona bene restringendo
                    purge(current_device);
                    ///L'UNLOCK QUI LO FA PURGE
                }
                else
                    spin_unlock(&lock[current_device]);
                DEBUG
                    printk("%s : ACTUAL_MAXIMUM_SIZE is %d\n",MODNAME,actual_maximum_size[current_device]);
            }
			else{
				spin_unlock(&lock[current_device]);
				return -EINVAL;
			}
			break;

		case ACTUAL_MAXIMUM_SEGMENT_SIZE_CTL:
			printk("%s: modifying the maximum segment size\n",MODNAME);
			if(arg<=MAX_SEGMENT_SIZE){ ///absoloute upper limit
				actual_maximum_segment_size[current_device] = arg;
				//anche qui ... se era pieno e ora restringo poi quando leggo tutto ok vedere??
                //devo traslare tutti i segmenti correnti !! conviene o li rialloco e memcpy e kfree
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
                printk("%s: getting the max segment size\n",    MODNAME);
            to_ret = actual_maximum_segment_size[current_device];
			spin_unlock(&lock[current_device]);
			return to_ret;
		case GET_FREE_SIZE:
            DEBUG
                printk("%s: getting the free size\n",MODNAME);
            to_ret = actual_maximum_size[current_device] - used[current_device];
			spin_unlock(&lock[current_device]);
			return to_ret;
        case PURGE_OPTION_CTL:
            DEBUG
                printk("%s: actual purge cmd\n",MODNAME);
            actual_purge_option[current_device] = arg;
            if(actual_purge_option[current_device]==PURGE) //magari avevo sforato e l'opzione era NO_PURGE.. ora PURGE!
                purge(current_device); ///L'unlock qui lo fa purge
            else
                spin_unlock(&lock[current_device]);
            break;
           //implementare il reset delle option
		default:
			printk("%s : DEFAULT\n",MODNAME);
			spin_unlock(&lock[current_device]);
			return -ENOTTY;
	}
	return 0;
}


static int ms_open(struct inode *inode, struct file *filp){
    int tmp_minor;
    atomic_inc(&count);//a new session
    tmp_minor=CURRENT_DEVICE;
    DEBUG
        printk("%s: somebody called an open  on mail slot  dev with minor number %d\n",MODNAME,tmp_minor);


    if (tmp_minor >= MAX_MINOR_NUM || tmp_minor < 0 ){ //check on device's minor number
        DEBUG
            printk("%s: error => somebody called an open  on mail slot  dev with minor number %d\n",MODNAME,tmp_minor);
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





static ssize_t ms_write(struct file *filp,const char *buff,size_t len,loff_t *off){
    int calling_device,res;
    segment * new;
    char * tmp; // per non rischiare di andare in sleep in copy from user
    DEBUG
        printk("%s: somebody called a write  on mail slot  dev with minor number %d\n",MODNAME,CURRENT_DEVICE);
    if(len > MAX_SEGMENT_SIZE){
        DEBUG
            printk("%s : Message cancelled. len=%zu, max segment size =%d \n",MODNAME,len,MAX_SEGMENT_SIZE);
        return -EMSGSIZE;
    }
    calling_device = CURRENT_DEVICE;
    //prealloco memoria qui cosi non sono in lock e non rischio di allungare la cs se per caso andassi in sleep
    tmp=kmalloc(len,GFP_KERNEL);
    memset(tmp,0,len);
    copy_from_user(tmp,buff,len);  //qui posso andare in sleep ma non fa niente non sono in cs



    new = kmalloc(sizeof(segment),GFP_KERNEL);
	memset(new,0,sizeof(segment));
	new->payload = kmalloc(len,GFP_KERNEL);
	memset(new->payload,0,len);

    spin_lock(&lock[calling_device]);

	///QUESTO DEVE ESSERE FATTO NECESSARIAMENTE GIA IN SEZIONE CRITICA. Le grandezze massime
	///POTREBBERO VARIARE ED INOLTRE IL BUFFER SI POTREBBE RIEMPIRE..

    //CHECK se il pacchetto è più grande della dimensione max, in questo caso fallisco
	if(len > actual_maximum_segment_size[calling_device]){
		printk("%s : Message cancelled. len=%zu, actual_maximum_size=%d \n",MODNAME,len, actual_maximum_segment_size[calling_device]);
		spin_unlock(&lock[calling_device]);
        //free fuori dalla sezione critica
        kfree(new->payload);
        kfree(new);
        return -EMSGSIZE;
	}

    //È OCCUPATO PIÙ DEL DISPONIBILE (MAX SIZE CAMBIATA) oppure non c'è spazio
    while((actual_maximum_size[calling_device])-used[calling_device] <= 0 || len>(actual_maximum_size[calling_device])-used[calling_device] ){
        if(blk_mode[calling_device]==NON_BLOCKING_MODE){
            spin_unlock(&lock[calling_device]);
            kfree(new->payload);
            kfree(new);
            return -EAGAIN;
        }
        spin_unlock(&lock[calling_device]);
        res = wait_event_interruptible(wait_queues[calling_device], len<=(actual_maximum_size[calling_device]-used[calling_device]));
        if(res!=0){
            printk("%s:the process with pid %d has been awaked by a signal\n",MODNAME,current->pid);
            kfree(new->payload);
            kfree(new);
            return -ERESTARTSYS;
        }
        else
            printk("%s: res is %d\n",MODNAME,res);
        spin_lock(&lock[calling_device]);
    }

	/*ALLOCAZIONE E RESET  //errore sono in lock non posso allocare potrei andare insleep
	//con kmalloc poiche non ce l ha chiede al buddy e non ce lha
	new = kmalloc(sizeof(node),0);
	memset(new,0,sizeof(node));
	new->payload = kmalloc(len,0);
	memset(new->payload,0,len);*/

	//COPIARE IL BUFFER NEL NODO
	//copy_from_user (new->payload,buff,len); //buff potrebbe essere non materializzato oppure empty zero e andrei in sleep
    memcpy(new->payload,tmp,len);  //è tutta memoria kernel
	new->size = len;
	new->next = NULL;
	if(head[calling_device]==NULL){
		head[calling_device] = new;
		tail[calling_device] = new;
	}
	else{
		tail[calling_device]->next = new;
		tail[calling_device] = new;
	}

	used[calling_device] += len;


	spin_unlock(&lock[calling_device]);

    wake_up(&wait_queues[calling_device]);


   return len;
}


static ssize_t ms_read(struct file * filp , char * buff , size_t  len , loff_t * off){
    DEBUG
        printk("%s: somebody called a read  on mail slot  dev with minor number %d\n",MODNAME,CURRENT_DEVICE);
    return 0;
}


static void purge(int current_device){
    segment* dead_list = NULL;
    segment* last = NULL; //ULTIMA DA CANCELLARE
    segment* to_die = NULL;
    int done = 0;
    while(used[current_device] > actual_maximum_size[current_device]){
        if(dead_list==NULL)
            dead_list = head[current_device];
        last  = head[current_device];
        used[current_device] -= head[current_device]->size;
        head[current_device] = head[current_device]->next;
        if(head[current_device]==NULL)
            tail[current_device] = NULL;
    }

    spin_unlock(&lock[current_device]);

    while(dead_list !=NULL && !done){
        if(dead_list ==last)
            done=1;
        to_die = dead_list;
        dead_list = dead_list->next;
        kfree(to_die->payload);
        kfree(to_die);
    }
}



static struct file_operations fops = {
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
