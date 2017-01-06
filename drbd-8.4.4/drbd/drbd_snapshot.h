#include "drbd_int.h"
#ifndef  __SNAPSHOT_H__
#define __SNAPSHOT_H__
//char *usermode_cmd="/usr/sbin/drbd_snapshot.sh";
extern struct proc_dir_entry* proc_sdrbd;
extern int drbd_khelper_usercmd(struct drbd_device *device, char *cmd);
//extern int drbd_send_snapshot_cmd(struct drbd_peer_device *peer_device,int cmd);
extern int snapshot_sync_setup(struct drbd_device *device);
extern struct proc_dir_entry* drbd_proc_create_device(char*name,struct drbd_device *device);
struct drbd_snapshot_device{
        unsigned int flags;
        wait_queue_head_t q_wait;
        struct drbd_bitmap *bitmap; // snapshot bitmap
        struct drbd_device*device;  // drbd device
        struct block_device*ldev;   // snapshot low device
        struct task_struct *tsk;
        
}; 
#endif