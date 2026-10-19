# Motorola penangf: from source to an Alpine desktop

This is the single build and installation guide. Run the command blocks in order.
The target is an unlocked Motorola penangf (tested XT2333-3), slot **b**.
The result is Linux 6.18, the ICNL9916 display/touch drivers, Panfrost, Alpine,
Weston and Fastfetch. Display modes are 60/90 Hz. OTG, charging and system suspend
are not complete; this guide does not claim a fully supported phone.

Use an Arch Linux x86_64 build PC, Internet access and at least 15 GiB free disk
space. Rootfs creation uses QEMU user-mode emulation. Commands assume Bash and
one terminal session; keep the exported variables until the build is complete.
Run commands in order and stop on any error before continuing. Do not enable
`set -e` in the interactive shell: it can close the terminal on a failed command.
The clone command requires these local changes to have been published to the
fork first. If working in the already prepared local checkout, skip cloning.
Nothing in this guide automatically pushes Git changes.

## 1. Install host dependencies

```bash
sudo pacman -Syu --needed base-devel git aarch64-linux-gnu-gcc \
  aarch64-linux-gnu-binutils aarch64-linux-gnu-glibc bc flex bison \
  openssl libelf python cpio gzip bzip2 curl file e2fsprogs util-linux \
  android-tools qemu-user-static qemu-user-static-binfmt inetutils
sudo systemctl restart systemd-binfmt.service
command -v aarch64-linux-gnu-gcc mkbootimg unpack_bootimg img2simg qemu-aarch64-static
```

Arch's [QEMU documentation](https://wiki.archlinux.org/title/QEMU) describes
[qemu-user-static-binfmt](https://archlinux.org/packages/extra/x86_64/qemu-user-static-binfmt/).
Do not skip this step, even if the compiler is already installed: ARM64 chroot
also needs QEMU and binfmt registration. A later `chroot /bin/true` check must
succeed before package installation.

## 2. Clone and set build variables

For a new checkout (choose an unused destination):

```bash
git clone --single-branch --branch 6.18 https://github.com/xaxaxa00/linux.git "$HOME/penangf-linux"
cd "$HOME/penangf-linux"
```

For the existing prepared checkout, use `cd "$HOME/blyat"` instead.
Start this full build with an empty `out-penangf` directory. If it contains an
older build, save any images you want to keep, then run `rm -rf out-penangf`
from the repository root before proceeding. If it contains a root-owned Alpine
staging tree, removal may require sudo. Unmount any staging `dev`, `proc` and
`sys` mounts before deleting it. Do not remove the directory between steps.
A previous attempt with stale output omitted the touch module; a fresh build
resolved it. The exact stale file responsible was not identified.

Then run:

```bash
export REPO="$PWD"
export OUT="$REPO/out-penangf"
export KERNELRELEASE=6.18.0-penangf
export CROSS_COMPILE=aarch64-linux-gnu-
export JOBS=4
mkdir -p "$OUT"
sha256sum -c penangf/ported-sources.sha256
sha256sum -c penangf/boot-assets.sha256
```

The base commit is `c75a6bdd2a48359f1f099e5dc26859a4e8a6613d`. It already contains
MediaTek platform support; this is not a vanilla upstream v6.18 tree.

## 3. Build kernel, DTB and modules, including the GPU driver

```bash
cp penangf/configs/kernel.config "$OUT/.config"
make O="$OUT" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" \
  KERNELRELEASE="$KERNELRELEASE" olddefconfig
make -j"$JOBS" O="$OUT" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" \
  KERNELRELEASE="$KERNELRELEASE" Image.gz modules \
  mediatek/mt6769z-motorola-penangf.dtb \
  mediatek/mt6769z-motorola-penangf-minimal-dtbo.dtb
```

Panfrost source is already in `drivers/gpu/drm/panfrost/`: no separate kernel GPU
repository is required. The supplied config has `CONFIG_DRM_PANFROST=m`.
This step builds `panfrost.ko` and `gpu-sched.ko` along with the display and touch
modules. They will be installed into rootfs below. Use modules from this exact
kernel build; matching release strings alone are insufficient.

## 4. Package the compiled overlay as Android DTBO

Linux DTB is packed inside boot.img. The bootloader also needs a separate Android
DTBO image. Step 3 already compiles the existing kernel source
`arch/arm64/boot/dts/mediatek/mt6769z-motorola-penangf-minimal-dtbo.dts`.
Package that compiled `.dtb` payload into the Android container:

```bash
python3 - <<'PY'
import os, pathlib, struct
out = pathlib.Path(os.environ['OUT'])
payload = (out / 'arch/arm64/boot/dts/mediatek/mt6769z-motorola-penangf-minimal-dtbo.dtb').read_bytes()
header = struct.pack('>8I', 0xd7b7ab1e, 64 + len(payload), 32, 32, 1, 32, 4096, 0)
entry = struct.pack('>8I', len(payload), 64, 0, 0, 0, 0, 0, 0)
image = header + entry + payload
image += bytes((-len(image)) % 4096)
(out / 'dtbo.img').write_bytes(image)
PY
printf '%s  %s\n' dfe8b83a9cb8599288833d3adb1535b2f32f40e6c5098cc68f11e8e68e35c612 \
  "$OUT/dtbo.img" | sha256sum -c -
```

The generated image matches the recorded working reference checksum. No separate
copy of the overlay source or prebuilt DTBO is needed under penangf/. Existing
unit-address/fixup warnings come from the preserved bootloader overlay. The
container follows the [Android DT table format](https://source.android.com/docs/core/architecture/dto/partitions).

## 5. Build static BusyBox

```bash
curl -fL https://busybox.net/downloads/busybox-1.37.0.tar.bz2 \
  -o "$OUT/busybox-1.37.0.tar.bz2"
printf '%s  %s\n' \
  3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4 \
  "$OUT/busybox-1.37.0.tar.bz2" | sha256sum -c -
if [ ! -d "$OUT/busybox-1.37.0" ]; then
  tar -xjf "$OUT/busybox-1.37.0.tar.bz2" -C "$OUT"
fi
cp penangf/configs/busybox.config "$OUT/busybox-1.37.0/.config"
make -C "$OUT/busybox-1.37.0" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" oldconfig
make -C "$OUT/busybox-1.37.0" -j"$JOBS" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE"
export BUSYBOX="$OUT/busybox-1.37.0/busybox"
file "$BUSYBOX"
```

The result must be a statically linked ARM aarch64 executable.

## 6. Assemble initramfs

The included `penangf/initramfs/` files set up USB ECM, load PHY/display/touch,
locate userdata, then switch to Alpine. The touch firmware is loaded into SRAM,
not flashed into the controller's persistent memory.

```bash
export INITROOT="$(mktemp -d "$OUT/initramfs.XXXXXX")"
mkdir -p "$INITROOT"/{bin,dev,proc,sys,tmp,root,www,newroot} \
  "$INITROOT/lib/modules/$KERNELRELEASE" "$INITROOT/lib/firmware/chipone"
install -m755 "$BUSYBOX" "$INITROOT/bin/busybox"
ln -s busybox "$INITROOT/bin/sh"
install -m755 penangf/initramfs/init penangf/initramfs/display-autostart.sh "$INITROOT/"
install -m644 penangf/firmware/chipone/ICNL9916.bin "$INITROOT/lib/firmware/chipone/"
install -m644 \
  "$OUT/drivers/gpu/drm/mediatek/mediatek-drm.ko" \
  "$OUT/drivers/phy/mediatek/phy-mtk-mipi-dsi-drv.ko" \
  "$OUT/drivers/input/touchscreen/chipone_icnl9916.ko" \
  "$INITROOT/lib/modules/$KERNELRELEASE/"
(cd "$INITROOT"; find . -print0 | cpio --null -o --format=newc --owner=0:0 --quiet) \
  | gzip -n > "$OUT/initramfs.cpio.gz"
```

Init discovers the partition whose sysfs uevent has `PARTNAME=userdata`,
without a phone-specific PARTUUID or fixed mmcblk number. It requires exactly
one matching partition and checks that it contains ext4 with filesystem UUID
`aa092c4f-72a7-44e4-bbfe-4e20b1462e4c`; otherwise it enters the USB rescue shell.
No personal UUID needs to be entered for another supported penangf phone.

PARTUUID identifies a GPT partition and may differ between devices. The filesystem
UUID above belongs to the image: step 10 explicitly assigns it with `mkfs.ext4 -U`,
and step 9 uses the same value in fstab. Keep these three values consistent if
changing the image UUID. This does not make the port compatible with other phone
models or hardware variants.

To update an existing installation that already uses this rootfs UUID, repeat
steps 6 and 7 and flash only the rebuilt boot image to its intended slot. There
is no need to recreate or flash userdata for this discovery change.

## 7. Pack boot.img

The header parameters are included; an old boot image is not required.

```bash
python3 - <<'PY'
import json, os, pathlib, subprocess
repo = pathlib.Path(os.environ['REPO'])
out = pathlib.Path(os.environ['OUT'])
args = json.loads((repo / 'penangf/boot/header-args.json').read_text())
inputs = {
    '--kernel': out / 'arch/arm64/boot/Image.gz',
    '--dtb': out / 'arch/arm64/boot/dts/mediatek/mt6769z-motorola-penangf.dtb',
    '--ramdisk': out / 'initramfs.cpio.gz',
}
for flag, path in inputs.items():
    if not path.is_file(): raise SystemExit(f'Missing {path}')
    args.extend([flag, str(path)])
image = out / 'boot-penangf.img'
subprocess.run(['mkbootimg', *args, '--output', str(image)], check=True)
assert image.stat().st_size <= 32 * 1024 * 1024, 'boot partition size exceeded'
check = out / 'boot-verify'
subprocess.run(['unpack_bootimg', '--boot_img', str(image), '--out', str(check)], check=True)
for flag, path in inputs.items():
    assert (check / flag[2:]).read_bytes() == path.read_bytes(), flag
PY
```

## 8. Download and prepare Alpine rootfs

Use a new staging directory. The package list is kept in `penangf/packages.txt`;
`rootfs-packages.tsv` is the inventory of the previous working system, not a lockfile.
The repository may update package versions over time.

If `$OUT/alpine-rootfs` already exists and you want to restart this step, set
`ROOTFS="$OUT/alpine-rootfs"`, unmount any staging `dev`, `proc` and `sys` mounts,
then run `sudo rm -rf "$ROOTFS"` and repeat the block. This deletes only the
staged rootfs; verify the variable points to the staging directory first.
If extraction succeeded and only the chroot check failed, fix binfmt and retry
just the final two commands instead of downloading and extracting again.

```bash
export ROOTFS="$OUT/alpine-rootfs"
ALPINE=alpine-minirootfs-3.24.2-aarch64.tar.gz
ALPINE_URL=https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64
curl -fL "$ALPINE_URL/$ALPINE" -o "$OUT/$ALPINE"
curl -fL "$ALPINE_URL/$ALPINE.sha256" -o "$OUT/$ALPINE.sha256"
(cd "$OUT"; sha256sum -c "$ALPINE.sha256")
sudo mkdir -p "$ROOTFS"
sudo tar -xpf "$OUT/$ALPINE" -C "$ROOTFS"
sudo cp -L /etc/resolv.conf "$ROOTFS/etc/resolv.conf"
sudo systemctl restart systemd-binfmt.service
sudo chroot "$ROOTFS" /bin/true
```

If the last command reports Exec format error, fix aarch64 binfmt registration
before proceeding. Do not ignore it.

## 9. Install Mesa, Weston, Fastfetch and startup files

The block below uses sudo only for the rootfs staging tree and temporary mounts.
It cleans up its mounts on exit. No phone partition is touched.

```bash
sudo env REPO="$REPO" OUT="$OUT" ROOTFS="$ROOTFS" BUSYBOX="$BUSYBOX" \
  KERNELRELEASE="$KERNELRELEASE" bash <<'ROOT'
set -euo pipefail
cd "$REPO"
[ "$ROOTFS" != / ] && [ -f "$ROOTFS/etc/alpine-release" ]
cleanup() {
  for d in sys proc dev; do
    if mountpoint -q "$ROOTFS/$d"; then umount "$ROOTFS/$d" || return 1; fi
  done
}
trap cleanup EXIT
mkdir -p "$ROOTFS"/{dev,proc,sys}
mount --bind /dev "$ROOTFS/dev"
mount -t proc proc "$ROOTFS/proc"
mount --bind /sys "$ROOTFS/sys"
printf '%s\n' \
  https://dl-cdn.alpinelinux.org/alpine/v3.24/main \
  https://dl-cdn.alpinelinux.org/alpine/v3.24/community > "$ROOTFS/etc/apk/repositories"
mapfile -t packages < penangf/packages.txt
chroot "$ROOTFS" /sbin/apk add "${packages[@]}"
cp -a penangf/rootfs/. "$ROOTFS/"
mkdir -p "$ROOTFS/usr/local/sbin" "$ROOTFS/lib/firmware/chipone" \
  "$ROOTFS/lib/modules/$KERNELRELEASE/extra" "$ROOTFS"/{run,tmp,www,var/log}
install -m755 "$BUSYBOX" "$ROOTFS/usr/local/sbin/busybox"
install -m644 penangf/firmware/chipone/ICNL9916.bin "$ROOTFS/lib/firmware/chipone/"
install -m644 "$OUT/drivers/gpu/drm/scheduler/gpu-sched.ko" \
  "$OUT/drivers/gpu/drm/panfrost/panfrost.ko" \
  "$ROOTFS/lib/modules/$KERNELRELEASE/extra/"
chmod 755 "$ROOTFS/etc/penangf/"*.sh "$ROOTFS/root/"*.sh
chmod 1777 "$ROOTFS/tmp"
chown -R 0:0 "$ROOTFS/etc/penangf" "$ROOTFS/root" \
  "$ROOTFS/lib/modules/$KERNELRELEASE" "$ROOTFS/usr/local/sbin/busybox"
printf '%s\n' 'UUID=aa092c4f-72a7-44e4-bbfe-4e20b1462e4c / ext4 defaults,noatime 0 1' > "$ROOTFS/etc/fstab"
cleanup
trap - EXIT
ROOT
```

Panfrost has two parts: the kernel modules built in step 3, and Mesa userspace
installed here through `mesa-dri-gallium`, EGL and GLES packages. No separate Mesa
build is needed to reproduce the working setup. Official source and build details:
[Mesa repository](https://gitlab.freedesktop.org/mesa/mesa),
[Panfrost documentation](https://docs.mesa3d.org/drivers/panfrost.html).
We did not write Panfrost or modify Mesa.

The startup files load gpu-sched then panfrost before Weston. Weston uses GL when
the render node exists, otherwise Pixman. Firmware is also installed in rootfs
because touch needs it again after panel power cycling.

## 10. Create the flashable rootfs image

Verify that no temporary mounts remain. These commands create image files on the
PC, not a new phone partition table. Increase 2G before mkfs if desired, staying
within the phone's userdata capacity. The subshell below stops on errors without
closing the interactive terminal. If it fails, resolve the error before flashing;
existing image files must be moved aside before repeating this step.

```bash
(
set -euo pipefail
for d in dev proc sys; do
  if mountpoint -q "$ROOTFS/$d"; then
    echo "Still mounted: $ROOTFS/$d; unmount it before creating the image."
    false
  fi
done
test ! -e "$OUT/rootfs.ext4"
test ! -e "$OUT/userdata.sparse.img"
truncate -s 2G "$OUT/rootfs.ext4"
sudo mkfs.ext4 -F -m 0 -L penangf-root \
  -U aa092c4f-72a7-44e4-bbfe-4e20b1462e4c -d "$ROOTFS" "$OUT/rootfs.ext4"
sudo e2fsck -fn "$OUT/rootfs.ext4"
img2simg "$OUT/rootfs.ext4" "$OUT/userdata.sparse.img"
(cd "$OUT"; sha256sum boot-penangf.img dtbo.img userdata.sparse.img > FLASH-SHA256SUMS)
(cd "$OUT"; sha256sum -c FLASH-SHA256SUMS)
)
```

You now have the three flashable files in out-penangf:
`boot-penangf.img`, `dtbo.img`, `userdata.sparse.img`.

## 11. Flash the phone

**This replaces userdata completely, deleting its existing Android/Linux data.**
Fastboot writes the sparse image to the partition as block data; it does not
install files into the previous filesystem. Therefore an existing F2FS, ext4 or
encrypted Android userdata does not need conversion first: the flashed image
provides the new ext4 filesystem and its UUID. The partition must be large enough
for the expanded image (2 GiB with the commands above), and fastboot must accept
the flash successfully. GPT and its PARTUUID are not recreated by this operation.
This is replacement, not secure erasure of every byte of the old partition.
Save your current working images first. The bootloader must already be unlocked.
Only connect the intended phone. Enter bootloader fastboot physically, then check:

```bash
fastboot devices
fastboot getvar product
fastboot getvar current-slot
fastboot getvar partition-size:userdata
```

Confirm it is the intended penangf. Then run each command in order and check for
OKAY. Stop on any failure. Slot a is not modified.

```bash
fastboot flash dtbo_b "$OUT/dtbo.img"
fastboot erase vendor_boot_b
fastboot -S 64M flash userdata "$OUT/userdata.sparse.img"
fastboot flash boot_b "$OUT/boot-penangf.img"
fastboot set_active b
fastboot reboot
```

The custom boot contains its own initramfs, so the working layout uses an empty
vendor_boot_b. Do not run fastboot -w or format unrelated partitions. For a later
kernel-only update, do not flash userdata; update matching GPU modules in the
existing rootfs before booting the new kernel.

## 12. Connect and open Fastfetch

On the PC, after Linux starts:

```bash
USB_IF=""
for p in /sys/class/net/*; do
  [ -f "$p/address" ] || continue
  if [ "$(cat "$p/address")" = 02:00:00:00:68:02 ]; then USB_IF="${p##*/}"; break; fi
done
if [ -z "$USB_IF" ]; then
  echo 'USB ECM not detected; reconnect the phone and repeat this block.'
else
  sudo ip addr replace 192.168.77.2/24 dev "$USB_IF" &&
    sudo ip link set "$USB_IF" up &&
    telnet 192.168.77.1
fi
```

The remaining commands run **inside the phone's telnet shell**:

```bash
uname -r
mount | grep ' / '
cat /proc/modules | grep -E 'panfrost|gpu_sched|chipone'
grep 'GL renderer:' /run/weston.log
XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-0 HOME=/root \
  weston-terminal --font-size=12 --shell=/root/show-fastfetch.sh \
  >/run/fastfetch-window.log 2>&1 &
```

You should see the Weston desktop and a normal Fastfetch terminal window.
The working GPU renderer is `Mali-G52 r1 MC2 (Panfrost)`; llvmpipe means CPU rendering.
Test touch and clean output. USB telnet is root without a password; keep it on the
direct USB link. Use `sync; reboot` for a normal restart.

## Source layout and experiments

`drivers/` contains the new native panel/touch drivers and DSI/OVL fixes.
`arch/arm64/boot/dts/mediatek/mt6769z-motorola-penangf.dts` is the consolidated board
configuration. `penangf/` contains only build inputs: configurations, firmware,
boot header metadata, runtime initramfs/rootfs files and package lists.
`out-penangf/` is generated and ignored by Git; do not force-add it.

Display bring-up progressed from firmware/simple-panel handoff to full power/reset/
command initialization. Raising DSI to 912 Mbps alone worsened the bands; fixing
MT6768 packet intervals removed them. The final panel requests 912 Mbps, and
calibrated modes measured 59.998/89.997 Hz. OVL interrupt cleanup fixed duplicate
vblank accounting. Touch required SRAM firmware upload and a DRM panel follower;
two-finger movement and release were verified. GPU bring-up moved Weston from
Pixman to Panfrost GL. Rootfs moved from RAM to userdata, with discovery
replacing unreliable fixed mmcblk numbering. Initial discovery used the test
phone's PARTUUID; it now uses the userdata partition name plus ext4/UUID checks.

OTG experiments remain excluded: the separate I2C investigation returned ambiguous
values/timeouts and did not establish a working Type-C/VBUS implementation.
The final port excludes temporary registration traces and the old panel-simple
clock experiment. Panfrost source itself is unchanged from the base.

## Validation limits

The user completed a fresh build, Alpine/QEMU rootfs preparation, image flashing
and boot on the test phone. Display, touch and Weston worked; the GL renderer was
`Mali-G52 r1 MC2 (Panfrost)`. The persistent rootfs mounted as ext4 from
`mmcblk0p63`, and USB ECM and telnet worked. The device name is an observation,
not a device path to hardcode in init. The DTBO matches the working
reference byte-for-byte.

The reported installation also flashed slot a, whereas the commands above target
slot b only. That extra action is recorded here, not added as a requirement;
this run does not independently validate a b-only installation with slot a left
unchanged. Host dependency setup was initially skipped; ARM64 binfmt had to be
fixed before chroot worked. A fresh output directory resolved the missing touch
module. The guide now addresses both issues and avoids exiting the interactive
shell on the reported failure paths.

Package versions can differ from the recorded inventory. Ten simultaneous touches,
cold boot, system suspend, charging, prolonged GPU stress and OTG remain unverified.

The subsequent PARTNAME-based discovery change has local simulated checks, but
has not yet been boot-tested on the phone. The successful installation reported
above used the earlier PARTUUID-based discovery.
