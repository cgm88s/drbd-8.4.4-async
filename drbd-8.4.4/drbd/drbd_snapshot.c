#include <linux/module.h>
#include <linux/drbd.h>
#include <linux/drbd_genl_api.h>
#include <linux/genl_magic_struct.h>
#include <linux/kthread.h>

#include "drbd_int.h"
#include "drbd_snapshot.h"
#include "drbd_wrappers.h"
#include "drbd_bitmap.h"
//cmd="start" , "end"
//drbd_snapshot.sh cmd ldevice drbddevice
extern unsigned long bm_count_bits(struct drbd_bitmap *b);
#define usermode_cmd "/usr/sbin/drbd_snapshot.sh"

struct proc_dir_entry*proc_sdrbd=NULL;

struct drbd_bitmap {
	struct page **bm_pages;
	spinlock_t bm_lock;

	/* see LIMITATIONS: above */

	unsigned long bm_set;       /* nr of set bits; THINK maybe atomic_t? */
	unsigned long bm_bits;
	size_t   bm_words;
	size_t   bm_number_of_pages;
	sector_t bm_dev_capacity;
	struct mutex bm_change; /* serializes resize operations */

	wait_queue_head_t bm_io_wait; /* used to serialize IO of single pages */

	enum bm_flag bm_flags;

	/* debugging aid, in case we are still racy somewhere */
	char          *bm_why;
	struct task_struct *bm_task;
};
unsigned long sdrbd_bitmap_weight(struct drbd_bitmap*bitmap){
    unsigned long weight,flags;
    spin_lock_irqsave(&bitmap->bm_lock, flags);
    weight = bitmap->bm_set;   //drbd_bm_total_weight(device);
    spin_unlock_irqrestore(&bitmap->bm_lock, flags);
    return weight;
};

int drbd_khelper_usercmd(struct drbd_device *device, char *cmd){
    char *envp[] = { "HOME=/",                              \
                     "TERM=linux",                          \
                     "PATH=/sbin:/usr/sbin:/bin:/usr/bin",  \
                     (char[20]) { }, /* address family */   \
                     (char[60]) { }, /* address */          \
                     NULL };
    
    char mb[12];
    char ldevice[128]={0};
    char *argv[] = {usermode_cmd, cmd,ldevice, mb, NULL };
    struct drbd_connection *connection = first_peer_device(device)->connection;
    //struct sib_info sib;
    int ret;
    if (current == connection->worker.task)
        set_bit(CALLBACK_PENDING, &connection->flags);
    snprintf(mb, 12, "minor-%d", device_to_minor(device));
    snprintf(ldevice, 128, "%s", device->ldev->disk_conf->backing_dev);
    //setup_khelper_env(connection, envp);	
    /* The helper may take some time.	
    * write out any unsynced meta data changes now 
    */	
    drbd_md_sync(device);
    drbd_info(device, "helper command: %s %s %s %s\n", usermode_cmd, cmd,ldevice, mb);
    //sib.sib_reason = SIB_HELPER_PRE;
    //sib.helper_name = cmd;
    //drbd_bcast_event(device, &sib);
    ret = call_usermodehelper(usermode_cmd, argv, envp, UMH_WAIT_PROC);
    if (ret)
        drbd_warn(device, "helper command: %s %s %s %s exit code %u (0x%x)\n",	usermode_cmd, cmd,ldevice, mb,(ret >> 8) & 0xff, ret);
    else		
        drbd_info(device, "helper command: %s %s %s %s exit code %u (0x%x)\n",	usermode_cmd, cmd, ldevice,mb,(ret >> 8) & 0xff, ret);
    //sib.sib_reason = SIB_HELPER_POST;
    //sib.helper_exit_code = ret;
    //drbd_bcast_event(device, &sib);
    if (current == connection->worker.task)		
        clear_bit(CALLBACK_PENDING, &connection->flags);
    //if (ret < 0) /* Ignore any ERRNOs we got. */	
    //   ret = 0;
    return ret;
}

int drbd_send_snapshot_cmd(struct drbd_peer_device *peer_device,int cmd)
{
    struct drbd_device *device = peer_device->device;
	struct drbd_socket *sock;
	struct p_sn_my *p;
	//int i;

	if (!get_ldev_if_state(device, D_NEGOTIATING))
		return 0;

	sock = &peer_device->connection->data;
	p = drbd_prepare_command(peer_device, sock);
	if (!p) {
		put_ldev(device);
		return -EIO;
	}
	
    p->flags=cmd; // just diff start or end 
	put_ldev(device);
	return drbd_send_command(peer_device, sock, cmd, sizeof(*p), NULL, 0);
}


static int sdrbd_sysctl_show(struct seq_file *seq, void *v)
{
    struct drbd_device* sdrbd=NULL;
    unsigned int flags=0;
    u64*uuid;
    struct net_conf*nc;
	sdrbd=(struct drbd_device *)seq->private;
	if((sdrbd->state.disk<D_INCONSISTENT)||(sdrbd->state.conn<C_CONNECTED)){
        //seq_printf(seq,"2:not support snapshot\n");
	}else if(sdrbd->ldev){
    	//__acquire(local);
        flags=sdrbd->ldev->md.flags;
        //__release(local);
        if(flags&(MDF_SNAPSHOT_SYNC_S|MDF_SNAPSHOT_SYNC_T)){
            seq_printf(seq,"1:snapshot sync running[%s]...\n",flags&MDF_SNAPSHOT_SYNC_S?"Source":"Target");
        }else{
            rcu_read_lock();
            nc=rcu_dereference(first_peer_device(sdrbd)->connection->net_conf);//
            if((sdrbd->state.role!=R_PRIMARY)||(nc&&(nc->wire_protocol!=DRBD_PROT_S))){
                seq_printf(seq,"2:not support snapshot\n");
            }else{
                seq_printf(seq,"0:normal [used:%d,rwmit:%ld]\n",sdrbd->lru_noblock->used,sdrbd->lru_noblock->starving);
                //lc_seq_printf_stats(seq, sdrbd->act_log);
            }
            rcu_read_unlock();
        }
        
        if(sdrbd->resync)
            lc_seq_printf_stats(seq, sdrbd->resync);
	    if(sdrbd->act_log)
	        lc_seq_printf_stats(seq, sdrbd->act_log);

	   seq_printf(seq,"debug_flag:%d\n",atomic_read(&sdrbd->debug_noblock));
	   uuid=sdrbd->ldev->md.uuid;
	   seq_printf(seq,"uuid %016llx:%016llx:%016llx:%016llx\n",\
	                    (unsigned long long)uuid[UI_CURRENT],\
	                    (unsigned long long)uuid[UI_BITMAP],\
	                    (unsigned long long)uuid[UI_HISTORY_START],\
	                    (unsigned long long)uuid[UI_HISTORY_END]);
	   seq_printf(seq,"peer %016llx:%016llx:%016llx:%016llx\n",\
	                    (unsigned long long)sdrbd->p_uuid[UI_CURRENT],\
	                    (unsigned long long)sdrbd->p_uuid[UI_BITMAP],\
	                    (unsigned long long)sdrbd->p_uuid[UI_HISTORY_START],
	                    (unsigned long long)sdrbd->p_uuid[UI_HISTORY_END]);
    }  
	return 0;
}
static int sdrbd_sysctl_open(struct inode *inode, struct file *file)
{
	return single_open(file, &sdrbd_sysctl_show, PDE_DATA(inode));
}

unsigned long make_snapshot_sync_request_send(struct drbd_snapshot_device*sdev,int cancel){
    unsigned long bit;	
    const sector_t capacity=drbd_get_capacity(sdev->ldev);
    int max_bio_size;
    unsigned long begin=0;
    int  size=0;
    sector_t sector;
    struct drbd_peer_request *peer_req;
    struct drbd_peer_device *peer_device=first_peer_device(sdev->device);
    unsigned long i=0,queued,sndbuf;
    unsigned long weight=0;
    unsigned long now;
    struct drbd_connection *connection=first_peer_device(sdev->device)->connection;
    struct drbd_bitmap *b = sdev->bitmap;
    struct drbd_device*device=sdev->device;
    struct net_conf*nc;
    int fault_type= DRBD_FAULT_RS_RD|DRBD_SNAP_DEV;
    
    if(unlikely(cancel)){
        return 0;
    }
    rcu_read_lock();
    nc=rcu_dereference(connection->net_conf);
    if(!nc||nc->wire_protocol!=DRBD_PROT_S){
        rcu_read_unlock();
        return 0;
    }   
    rcu_read_unlock();
    if(device->state.conn<C_CONNECTED){
        return 0;
    }
    max_bio_size=queue_max_hw_sectors(sdev->device->rq_queue) << 9;  // 1M
        
    weight = sdrbd_bitmap_weight(b);  //b->bm_set;   //drbd_bm_total_weight(device);
       
    if(weight<1)
        return 0;
    for(i=0;i<weight;i++){
        if(kthread_should_stop()){
            return i;
        }
        if(!get_ldev(sdev->device)){ //
            drbd_err(device, "%s:Disk broke down during sync!\n",__FUNCTION__);
            return 0;
        } 
        
    slow_snapsync:

        if(!(device->ldev->md.flags&MDF_SNAPSHOT_SYNC_S)){
            drbd_info(device,"snapshot sync thread cancel by clear flags !\n");
            put_ldev(device);
            return 0;
        }
                //noblock_conn_check
        if((device->state.conn < C_CONNECTED)){ // connection close
            drbd_err(device, "%s:connection is broke up during sync snapshot!\n",__FUNCTION__);
            put_ldev(sdev->device);
            return 0;
        }
    
        mutex_lock(&first_peer_device(device)->connection->data.mutex);
		if (first_peer_device(device)->connection->data.socket) {
			queued = first_peer_device(device)->connection->data.socket->sk->sk_wmem_queued;
			sndbuf = first_peer_device(device)->connection->data.socket->sk->sk_sndbuf;
		} else {
			queued = 1;
			sndbuf = 0;
		}
		mutex_unlock(&first_peer_device(device)->connection->data.mutex);
		if (queued > ((sndbuf *8)/10)){ 
            set_current_state(TASK_INTERRUPTIBLE);
		    schedule_timeout(SLEEP_TIME);// 100ms 
		    goto slow_snapsync;
		}
       
     
        now=jiffies;
     next_sector:	
        size=BM_BLOCK_SIZE;  // 1<<12==4K
        
        bit=_sdrbd_bm_find_next(sdev,begin);
        if (bit == DRBD_END_OF_BITMAP) {
            put_ldev(sdev->device);
            return DRBD_END_OF_BITMAP;
        }		
        sector = BM_BIT_TO_SECT(bit);
        begin=bit+1;
        if(sdrbd_bm_test_bit(sdev,bit)==0){
            goto next_sector;
        }
        while(1){
            if(size+BM_BLOCK_SIZE>max_bio_size)
                break;
            if(sdrbd_bm_test_bit(sdev,bit+1)!=1)
                break;	
            bit++;		
            size+=BM_BLOCK_SIZE;
            i++;	
        }		
        if(size>BM_BLOCK_SIZE)	
            begin=bit+1;
        if(sector+(size>>9)>capacity)
            size=(capacity-sector)<<9;	
        peer_req = drbd_alloc_peer_req(peer_device, ID_SYNCER, sector, size,true,GFP_NOIO);	
        if(!peer_req){
            put_ldev(sdev->device);	
            return -ENOMEM;	
        }	
        peer_req->w.cb=w_e_end_rsdata_req;								
                //atomic_inc(device->rs_pending_cnt) ;  
                // 收到cmd 23响应时，清位图，dec_rs_pending(device);
                //drbd_send_block(peer_device,P_RS_DATA_REPLY,peer_req);  //cmd2
                // dec_unacked(device);	
        //if(drbd_rs_begin_io(device,sector)){
        //        goto out_free_e;
        //}        
        atomic_add(size >> 9, &sdev->device->rs_sect_ev);
        inc_unacked(device);
        spin_lock_irq(&device->resource->req_lock);	
        list_add_tail(&peer_req->w.list, &device->read_ee);
        spin_unlock_irq(&device->resource->req_lock);
        if (drbd_submit_peer_request(device, peer_req, READ, fault_type) != 0){ // read io
                goto submit_failed ;
        }
        //put_ldev(sdev->device);
        
    }
    //put_ldev(device);
    return i; 
submit_failed:	/* don't care for the reason here */
        drbd_err(sdev->device, "%s,submit failed, triggering re-connect\n",__FUNCTION__);	
        spin_lock_irq(&sdev->device->resource->req_lock);	
        list_del(&peer_req->w.list);
        spin_unlock_irq(&sdev->device->resource->req_lock);
        put_ldev(sdev->device);
        //printk(KERN_INFO" 10:%s: sync_noblock thread quit...i=%d.\n",__FUNCTION__,atomic_read(&device->local_cnt));
        drbd_free_peer_req(sdev->device, peer_req);
        return -EIO;
}
static int drbd_snapshot_sync_running(void*arg){
    struct drbd_device*device=(struct drbd_device*)arg;
    struct drbd_snapshot_device *sdev;
    struct block_device *bdev;
    unsigned long wants;
    struct page**npages;
    char snapdev[128]={0};
    unsigned long i=0;
    if(!get_ldev(device)){  // fix bug on doing snapshot but raid failed,
            return 0;//goto out;//
    }
    sdev = kzalloc(sizeof(struct drbd_snapshot_device), GFP_KERNEL);
	if (!sdev){
	    drbd_err(device,"Get memory failed snapshot device!\n");
	    put_ldev(device);
		return ERR_NOMEM;
    }
    init_waitqueue_head(&sdev->q_wait);
    sdev->device=device;
    device->sdev=sdev;
    sdev->tsk=current;
    //sdev.bitmap=device->snap_bitmap;
    sdev->bitmap=kzalloc(sizeof(struct drbd_bitmap),GFP_KERNEL);
    if(!sdev->bitmap){
        drbd_err(device,"Get memory failed for snapshot sync !\n");
        goto failed;
    }else{
        spin_lock_init(&sdev->bitmap->bm_lock);
	    mutex_init(&sdev->bitmap->bm_change);
	    init_waitqueue_head(&sdev->bitmap->bm_io_wait);
    }
   
    sprintf(snapdev,"/etc/drbd-snapshot-minor-%d",device_to_minor(device));
    bdev=blkdev_get_by_path(snapdev,FMODE_READ | FMODE_WRITE | FMODE_EXCL, device);
    if(IS_ERR(bdev)){
        drbd_err(device,"Open snapshot device %s failed!\n",snapdev);
        goto snap_failed;
    }
    sdev->ldev=bdev;
    
    wait_event_interruptible(sdev->q_wait,((0==atomic_read(&device->rs_pending_cnt)&&(0==atomic_read(&device->unacked_cnt)))||(kthread_should_stop())));
    if(kthread_should_stop()){
           goto fail_bitmap ;
    }
    
    sdev->flags=1;
    drbd_info(device,"Start do snapshot device sync ...[lo:%d,pe:%d,ua=%d,ap=%d]\n", \
                        atomic_read(&device->local_cnt),\
                        atomic_read(&device->ap_pending_cnt)+atomic_read(&device->rs_pending_cnt),\
                        atomic_read(&device->unacked_cnt),\
                        atomic_read(&device->ap_bio_cnt));
    
    drbd_rs_cancel_all(device);
    
    wants=device->bitmap->bm_number_of_pages;

    sdev->bitmap->bm_flags=device->bitmap->bm_flags;
    npages=bm_realloc_pages(sdev->bitmap,wants);
    if (!npages) {
	    drbd_err(device,"Get memory failed for snapshot bitmap!\n");
        goto fail_bitmap;
	}
    sdev->bitmap->bm_pages=npages;
    sdev->bitmap->bm_number_of_pages=device->bitmap->bm_number_of_pages;
    sdev->bitmap->bm_bits=device->bitmap->bm_bits;
    sdev->bitmap->bm_words=device->bitmap->bm_words;
    sdev->bitmap->bm_dev_capacity=device->bitmap->bm_dev_capacity;
    //mutex_lock(&sdev->bitmap->bm_change);
    if(bm_rw_snapdevice(sdev,READ,0,0)){
        drbd_err(device,"Failed to read bitmap from snapshot device!\n");
        goto out;
    }
    //mutex_unlock(&sdev->bitmap->bm_change);
    
    //sdev->bitmap->bm_set = bm_count_bits(sdev->bitmap); // make sure again,
    while(sdev->bitmap->bm_set > 0){
        if(kthread_should_stop()){
            goto out;
        }
       
        i=make_snapshot_sync_request_send(sdev,0); //do snap sync
        //sdev->bitmap->bm_set=bm_count_bits(sdev->bitmap);
        
        if((i>=0)&&(sdrbd_bitmap_weight(sdev->bitmap)<1)){
            break;
        }else{
            if(!(device->ldev->md.flags&MDF_SNAPSHOT_SYNC_S)){
                drbd_info(device,"snapshot sync thread exit because task cancel!\n");
                goto out;
            }
             if(kthread_should_stop()){
                goto out;
            }
            set_current_state(TASK_INTERRUPTIBLE);
            schedule_timeout(5*HZ);
        }
       
        
    }

//complete:
    drbd_info(device,"snapshot device sync complete !\n");
    drbd_send_snapshot_cmd(first_peer_device(device),P_SNAP_END);
    
    //device->ldev->md.flags&= ~MDF_SNAPSHOT_SYNC_S;
    //drbd_md_sync(device);
out:
    bm_free_pages(npages,wants);
    
fail_bitmap:
        blkdev_put(bdev,FMODE_READ | FMODE_WRITE | FMODE_EXCL);
snap_failed:        
        kfree(sdev->bitmap);
failed:
        kfree(sdev);
        device->sdev=NULL; 
        put_ldev(device);
        return 0;
}
// create and wake up snapshot thread...
int snapshot_sync_setup(struct drbd_device *device){   
    struct task_struct *nt;
    //int ret=0;
    if(device->sdev!=NULL){
        drbd_info(device,"thread drbd_snapsync_drbd%d has already started!\n",device_to_minor(device));
        return 0;
    }
    nt=kthread_create(drbd_snapshot_sync_running,device,"drbd_snapsync_drbd%d",device_to_minor(device));
    if(IS_ERR(nt)){
        drbd_err(device,"Couldn't start drbd_snapshot thread!\n");
        return -1;
    }
    wake_up_process(nt);
    drbd_info(device,"start drbd_snapshot_drbd%d thread!\n",device_to_minor(device));
    return 0;
}
static ssize_t sdrbd_sysctl_write(struct file *filp, const char __user *buffer, size_t length, loff_t *offset ){
    char cmd[128];
    struct drbd_device*sdrbd=NULL;
    unsigned int flags=0;
    int ret=0;
    struct net_conf *nc;
    if(copy_from_user(cmd,buffer,length)){
        return -EFAULT;
    }else{
        cmd[length]=0;
    }
    sdrbd=((struct seq_file *)filp->private_data)->private;
    if(!get_ldev(sdrbd)){   //disk failed
        return length;    
    }
    if(0==strncmp(cmd,"sbegin",6)){
        drbd_send_snapshot_cmd(first_peer_device(sdrbd),P_SNAP_START);
        goto out;

    }
    if(0==strncmp(cmd,"send",4)){
        drbd_send_snapshot_cmd(first_peer_device(sdrbd),P_SNAP_END);
        goto out;
    }
    if(0==strncmp(cmd,"cbegin",6)){
        __acquire(local);
        drbd_md_clear_flag(sdrbd,MDF_SNAPSHOT_SYNC_S);//clean flags MDF_SNAPSHOT_SYNC
        __release(local);
        drbd_md_sync(sdrbd);  
        goto out;
    }
    if(0==strncmp(cmd,"cend",4)){
        __acquire(local);
        drbd_md_clear_flag(sdrbd,MDF_SNAPSHOT_SYNC_T);//clean flags MDF_SNAPSHOT_SYNC
        __release(local);
        drbd_md_sync(sdrbd);  
        goto out;
    }
    if(0!=strncmp(cmd,"snapshot",8)){
        drbd_err(sdrbd,"sdrbd receive not support cmd:%s!",cmd);
        goto out;
    } 
    nc=rcu_dereference(first_peer_device(sdrbd)->connection->net_conf);
    if(nc&&nc->wire_protocol!=DRBD_PROT_S){
        drbd_err(sdrbd,"[%c]snapshot only support in protocol S!\n",nc->wire_protocol-DRBD_PROT_A+'A');
        goto out;
    }
    if(sdrbd->state.role!=R_PRIMARY){
        drbd_err(sdrbd,"snapshot only support in primary!\n");
        goto out;
    }
    flags=sdrbd->ldev->md.flags;
    if((flags&MDF_SNAPSHOT_SYNC_S)||(flags&MDF_SNAPSHOT_SYNC_T)){
        pr_info("snapshot sync not complete! not permit to snapshot again!\n");
    }else{ // snapshot 
        //__acquire(local);
        //sdrbd->ldev->md.flags|=MDF_SNAPSHOT_SYNC_S; //set flags MDF_SNAPSHOT_SYNC 
        //__release(local);
        
        //drbd_suspend_io(sdrbd);  //stop front IO
        drbd_bm_lock(sdrbd,"snapshot",BM_LOCKED_MASK);
        drbd_bm_write(sdrbd); //flush btimap to device, maybe bm_change_bits_to() doing
        drbd_bm_unlock(sdrbd);
        
        drbd_md_sync(sdrbd);
        ret=drbd_khelper_usercmd(sdrbd,"start");// do snapshot local
        if(ret){ // failed 
            //__acquire(local);
            //sdrbd->ldev->md.flags&= ~MDF_SNAPSHOT_SYNC_S;
            //__release(local);
            //drbd_md_sync(sdrbd);
            drbd_err(sdrbd,"helper command: start local snapshot failed!\n");
            printk(KERN_ERR"EVENT[0x00000000] helper command:start do snapshot failed!\n");
            //drbd_resume_io(sdrbd);
        }else{ // wake up snapshot syncing to peer

            __acquire(local);
            drbd_md_set_flag(sdrbd,MDF_SNAPSHOT_SYNC_S);//sdrbd->ldev->md.flags|=MDF_SNAPSHOT_SYNC_S; //set flags MDF_SNAPSHOT_SYNC 
            __release(local);
            
            drbd_md_sync(sdrbd);
            //drbd_bm_clear_all(sdrbd); //clear all,maybe lose some data in peer
            drbd_bm_clear_unchanged(sdrbd); // clear saved in snapshot
            //drbd_resume_io(sdrbd); // start front io
            mutex_lock(&sdrbd->resource->adm_mutex);
            if (drbd_request_state(sdrbd, NS(user_isp, 1)) != SS_NOTHING_TO_DO){
                drbd_info(sdrbd,"sync pause because while do snapshot device sync!\n");
            }
            mutex_unlock(&sdrbd->resource->adm_mutex);
            
            drbd_send_snapshot_cmd(first_peer_device(sdrbd),P_SNAP_START);
            snapshot_sync_setup(sdrbd);
        }
    }
  out:
    put_ldev(sdrbd);
    return length;
    
}
static struct file_operations sdrbd_proc_fops = {
	.open		= sdrbd_sysctl_open,
	.read		= seq_read,
	.write      = sdrbd_sysctl_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

struct proc_dir_entry* drbd_proc_create_device(char*name,struct drbd_device *device){
    struct proc_dir_entry*sdrbd_device_proc;
    if(!proc_sdrbd){
            proc_sdrbd=proc_mkdir("sdrbd",NULL);
            if(!proc_sdrbd){
                drbd_err(device,"Create snapshot root proc failed!\n");
                return NULL;
            }
    }
    sdrbd_device_proc=proc_create_data(name,0666,proc_sdrbd,&sdrbd_proc_fops,device);
    if(!sdrbd_device_proc){
        drbd_err(device,"Create snapshot proc %s failed!\n",name);
        return NULL;
    }
    return sdrbd_device_proc;
}
