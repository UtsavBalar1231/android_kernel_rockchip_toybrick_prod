#!/bin/sh

# Root file system partition has mounted on /sysroot,
# do something for backup your data and rescue system!
#
# e.g.
# 1) Mount sd card on /mnt: mount /dev/mmcblkop1 /mnt
# 2) Copy important data to /mnt: cp -r /sysroot/home/toybrick/<data> /mnt
# 3) Umount sd card: umount /mnt
#
# Add here to backup your files or fix system boot issue when rootfs crash

######### step1: create directory: /rootfs ########
# mkdir /rootfs

######## step2: mount rootfs partition ########
# In dual system, rootfs partition is /dev/mmcblk1p16
# mount /dev/mmcblk1p16 /rootfs
# In Linux system, rootfs partition is /dev/mmcblk1p4
# mount /dev/mmcblk1p4 /rootfs

######## step3: save or fix #########
# Do something

######## step4: umount rootfs partition ########
# umount rootfs
