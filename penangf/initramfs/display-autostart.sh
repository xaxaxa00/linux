#!/bin/sh
# Early display startup with native ICNL9916 panel initialization.
export PATH=/bin
log() {
    echo "$*"
    echo "<6>PENANGF BOOT: $*" > /dev/kmsg
}
fail() {
    log "ERROR: $*; USB diagnostic shell remains available if the kernel is responsive"
    exit 1
}
REL="$(uname -r)"
MOD="/lib/modules/$REL"
log "loading MIPI DSI PHY"
insmod "$MOD/phy-mtk-mipi-dsi-drv.ko" || fail "MIPI PHY load failed"
log "loading MediaTek DRM"
insmod "$MOD/mediatek-drm.ko" || fail "DRM load failed"
[ -c /dev/fb0 ] && [ -c /dev/dri/card0 ] || fail "display devices missing"
log "display ready: $(cat /sys/class/graphics/fb0/virtual_size)"
# Present actual recent kernel messages that preceded framebuffer takeover.
if [ -c /dev/tty1 ]; then
    dmesg | tail -n 80 > /dev/tty1
    printf '\n=== PENANGF LINUX: DISPLAY READY ===\nUSB shell: telnet 192.168.77.1\n\n' > /dev/tty1
fi
log "startup complete; console=tty0 continues showing kernel messages"
