#!/bin/sh
export PATH=/usr/local/sbin:/usr/sbin:/usr/bin:/sbin:/bin
hostname penangf
mount -t tmpfs -o mode=0755 tmpfs /run
mount -t tmpfs -o mode=1777 tmpfs /tmp
mkdir -p /www /var/log /run/user/0
chmod 700 /run/user/0
ln -sf /proc/mounts /etc/mtab
echo 'penangf persistent Alpine Linux' > /etc/issue.net
dmesg > /var/log/boot-dmesg.log
