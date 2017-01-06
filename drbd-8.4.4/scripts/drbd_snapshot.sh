#!/bin/sh
BACKING_BDEV=$2
cmd=$1 
drbd_device=$3

LVC_OPTIONS=" "
set_vg_lv_size()
{
        local X
        if ! X=$(lvs --noheadings --nosuffix --units s -o vg_name,lv_name,lv_size "$BACKING_BDEV") ; then
                echo "Cannot create snapshot of $BACKING_BDEV, apparently no LVM LV."
                return 1
        fi
        set -- $X
        VG_NAME=$1 LV_NAME=$2 LV_SIZE_K=$[$3 / 2]
        return 0
}
set_vg_lv_size || exit 0 # clean exit if not an lvm lv
DATE=$(date +"%Y-%m-%d-%H-%M-%S")
SNAP_ADDITIONAL=102400	
SNAP_NAME=$LV_NAME-$DATE
SNAP_SIZE=$(( SNAP_ADDITIONAL + LV_SIZE_K / 100))
if [[ $cmd == *end* ]];then
	 lvremove -f $VG_NAME/$SNAP_NAME
	exit 0;
else
	 lvcreate -s -n $SNAP_NAME -L ${SNAP_SIZE}k $LVC_OPTIONS $VG_NAME/$LV_NAME
	RETVAL=$?
	ln -fs /dev/$VG_NAME/$SNAP_NAME  /etc/drbd-snapshot-$drbd_device
	exit $RETVAL
fi