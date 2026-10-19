#!/bin/sh
set -eu
export PATH=/usr/local/sbin:/usr/sbin:/usr/bin:/sbin:/bin
export HOME=/root XDG_RUNTIME_DIR=/run/user/0 LIBSEAT_BACKEND=seatd WAYLAND_DISPLAY=wayland-0
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
module_dir=/lib/modules/$(uname -r)/extra
if [ ! -d /sys/module/gpu_sched ]; then insmod "$module_dir/gpu-sched.ko" || echo 'GPU scheduler load failed'; fi
if [ ! -d /sys/module/panfrost ]; then insmod "$module_dir/panfrost.ko" || echo 'Panfrost load failed'; fi
renderer=pixman
if [ -c /dev/dri/renderD128 ] && [ -d /sys/module/panfrost ]; then renderer=gl; fi
echo "Starting Weston renderer=$renderer"
if ! pgrep -x udevd >/dev/null; then udevd --daemon; fi
udevadm trigger --action=add
udevadm settle
if ! pgrep -x seatd >/dev/null; then
 seatd -g root >/run/seatd.log 2>&1 &
 sleep 1
fi
exec weston --backend=drm --renderer="$renderer" --drm-device=card0 --idle-time=0 --socket=wayland-0 --log=/run/weston.log
