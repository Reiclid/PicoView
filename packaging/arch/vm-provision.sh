#!/usr/bin/env bash
# Turn a booted Arch install medium into a machine that runs PicoView under
# Hyprland, with nothing to answer along the way.
#
# This exists so the Wayland front end can be tested on the compositor it is
# actually meant for. WSLg is Weston: it shows that the window opens and the
# pixels are right, but it says nothing about decorations, full screen or how a
# resize behaves on wlroots - and those are exactly the parts a compositor
# decides.
#
# From the live ISO, as root:
#
#   curl -sL https://raw.githubusercontent.com/Reiclid/PicoView/main/packaging/arch/vm-provision.sh | bash
#
# It wipes /dev/sda without asking. That is the point - it is for a scratch
# virtual machine and nothing else.
set -euo pipefail

DISK=${DISK:-/dev/sda}
USERNAME=${USERNAME:-dev}
PASSWORD=${PASSWORD:-dev}
HOSTNAME=${HOSTNAME:-picoview-vm}

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }

say "checking the network"
ping -c1 -W5 geo.mirror.pkgbuild.com >/dev/null || { echo "no network"; exit 1; }
timedatectl set-ntp true || true

say "partitioning $DISK"
sgdisk --zap-all "$DISK"
sgdisk -n1:0:+512M -t1:ef00 -c1:EFI "$DISK"
sgdisk -n2:0:0     -t2:8300 -c2:root "$DISK"
partprobe "$DISK" || true
sleep 2
mkfs.fat -F32 "${DISK}1"
mkfs.ext4 -F "${DISK}2"
mount "${DISK}2" /mnt
mkdir -p /mnt/boot
mount "${DISK}1" /mnt/boot

say "installing the system (this is the long part)"
# Everything needed to build the front end and to run a wlroots compositor on
# a machine with no graphics card worth the name. sway comes along as a second
# opinion: if Hyprland will not start on this virtual GPU, sway's software
# renderer still answers the questions about wlroots behaviour.
pacstrap -K /mnt \
    base linux linux-firmware \
    sudo nano vim networkmanager openssh \
    base-devel git meson ninja pkgconf \
    wayland wayland-protocols libxkbcommon \
    mesa vulkan-swrast \
    hyprland sway foot \
    grim wl-clipboard \
    xorg-xwayland

genfstab -U /mnt >> /mnt/etc/fstab

say "configuring"
cat > /mnt/root/stage2.sh <<STAGE2
set -euo pipefail
ln -sf /usr/share/zoneinfo/Europe/Kyiv /etc/localtime
hwclock --systohc
sed -i 's/^#en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen
sed -i 's/^#uk_UA.UTF-8/uk_UA.UTF-8/' /etc/locale.gen
locale-gen
echo 'LANG=en_US.UTF-8' > /etc/locale.conf
echo '$HOSTNAME' > /etc/hostname

useradd -m -G wheel -s /bin/bash '$USERNAME'
echo '$USERNAME:$PASSWORD' | chpasswd
echo 'root:$PASSWORD' | chpasswd
echo '%wheel ALL=(ALL:ALL) NOPASSWD: ALL' > /etc/sudoers.d/wheel

systemctl enable NetworkManager
systemctl enable sshd

# Straight into a session on boot: there is no one at this keyboard.
mkdir -p /etc/systemd/system/getty@tty1.service.d
cat > /etc/systemd/system/getty@tty1.service.d/autologin.conf <<'AUTO'
[Service]
ExecStart=
ExecStart=-/usr/bin/agetty --autologin $USERNAME --noclear %I \$TERM
AUTO

bootctl install
cat > /boot/loader/loader.conf <<'LOADER'
default arch.conf
timeout 1
console-mode max
editor no
LOADER
ROOTUUID=\$(blkid -s UUID -o value ${DISK}2)
cat > /boot/loader/entries/arch.conf <<ENTRY
title   Arch Linux
linux   /vmlinuz-linux
initrd  /initramfs-linux.img
options root=UUID=\$ROOTUUID rw
ENTRY
STAGE2

arch-chroot /mnt bash /root/stage2.sh
rm -f /mnt/root/stage2.sh

say "setting up the session"
install -d -o 1000 -g 1000 /mnt/home/$USERNAME/.config/hypr
cat > /mnt/home/$USERNAME/.config/hypr/hyprland.conf <<'HYPR'
# Deliberately plain. The point is to see what the compositor does to a window,
# not to look at a rice.
monitor = , 1440x900@60, 0x0, 1

general {
    gaps_in = 4
    gaps_out = 8
    border_size = 2
    layout = dwindle
}
decoration {
    rounding = 6
}
animations {
    enabled = false      # a software renderer has better things to do
}
misc {
    disable_hyprland_logo = true
    disable_splash_rendering = true
    force_default_wallpaper = 0
}
input {
    kb_layout = us
    follow_mouse = 1
}

$mod = SUPER
bind = $mod, Return, exec, foot
bind = $mod, Q, killactive
bind = $mod, F, fullscreen, 0
bind = $mod, M, exit
bind = $mod, Left, movefocus, l
bind = $mod, Right, movefocus, r

# Something to look at the moment it comes up. Through a shell, because
# exec-once does not expand a tilde, and harmless before the first build.
exec-once = bash -lc 'picoview ~/pics'
HYPR

# A shell that lands in Hyprland on tty1 and stays a plain shell anywhere else.
cat > /mnt/home/$USERNAME/.bash_profile <<'PROFILE'
[[ -f ~/.bashrc ]] && . ~/.bashrc
if [[ -z $WAYLAND_DISPLAY && $XDG_VTNR == 1 ]]; then
    export XDG_RUNTIME_DIR=/run/user/$UID
    export XDG_SESSION_TYPE=wayland
    export LIBGL_ALWAYS_SOFTWARE=1      # there is no real GPU in here
    export WLR_RENDERER_ALLOW_SOFTWARE=1
    export WLR_NO_HARDWARE_CURSORS=1
    exec Hyprland
fi
PROFILE

say "building PicoView"
cat > /mnt/home/$USERNAME/build.sh <<'BUILD'
#!/usr/bin/env bash
# Fetch or update the source and build the Wayland front end. Safe to re-run.
set -euo pipefail
cd ~
if [[ -d PicoView/.git ]]; then
    git -C PicoView fetch --all --quiet
    git -C PicoView reset --hard origin/main --quiet
else
    git clone --depth 1 https://github.com/Reiclid/PicoView.git
fi
cd PicoView
rm -rf build
meson setup build linux
meson compile -C build
sudo install -Dm755 build/picoview /usr/local/bin/picoview
echo "picoview installed: $(picoview --help | head -1)"
BUILD
chmod +x /mnt/home/$USERNAME/build.sh

# Test material that needs nothing downloaded: a few pictures written by a
# program, and an empty file with a music extension - the generated cover art
# is made from the name alone and never opens the file.
cat > /mnt/home/$USERNAME/mkpics.c <<'MKPICS'
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
/* Binary PNM, which stb_image reads, so no encoder is needed to make one. */
static void shot(const char* path, int w, int h, int kind) {
    FILE* f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double u = (double)x / w, v = (double)y / h;
            int r, g, b;
            if (kind == 0) { r = (int)(255*u); g = (int)(255*v); b = 200; }
            else if (kind == 1) {
                int cx = (x/48 + y/48) & 1;
                r = cx ? 235 : 30; g = cx ? 235 : 30; b = cx ? 235 : 30;
            } else {
                double d = hypot(u-0.5, v-0.5) * 3.0;
                r = (int)(255*fabs(sin(d*4))); g = (int)(255*fabs(sin(d*4+2))); b = (int)(255*fabs(sin(d*4+4)));
            }
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    fclose(f);
}
int main(void) {
    shot("pics/01-gradient.ppm", 1600, 1100, 0);
    shot("pics/02-checks.ppm",    900,  900, 1);
    shot("pics/03-rings.ppm",    2400, 1600, 2);
    return 0;
}
MKPICS

cat > /mnt/home/$USERNAME/mkpics.sh <<'MK'
#!/usr/bin/env bash
set -euo pipefail
cd ~
mkdir -p pics
cc -O2 -o /tmp/mkpics mkpics.c -lm
(cd ~ && /tmp/mkpics)
: > ~/pics/Jenny.mp3        # the cover art is made from the name, not the file
: > ~/pics/beat.wav
ls -l ~/pics
MK
chmod +x /mnt/home/$USERNAME/mkpics.sh

arch-chroot /mnt chown -R $USERNAME:$USERNAME /home/$USERNAME

say "done - rebooting into Hyprland"
echo "user: $USERNAME  password: $PASSWORD"
echo "after the first boot, run:  ./mkpics.sh && ./build.sh"
umount -R /mnt
systemctl reboot
