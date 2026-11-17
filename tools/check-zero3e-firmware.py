#!/usr/bin/env python3
"""Inspect the built firmware and ext4 contents without mounting or flashing it."""

import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def check(sdk):
    firmware = sdk / "output/firmware"
    br = sdk / "buildroot/output/rockchip_rk3566_zenith"
    config = (br / ".config").read_text().splitlines()
    for setting in (
        'BR2_TARGET_GENERIC_HOSTNAME="zenith-zero3e"',
        'BR2_TARGET_ENABLE_ROOT_LOGIN=y', 'BR2_TARGET_GENERIC_ROOT_PASSWD=""',
        'BR2_TARGET_SERIAL_SHELL_SH=y',
        'BR2_TOOLCHAIN_BUILDROOT_GLIBC=y', 'BR2_TOOLCHAIN_BUILDROOT_CXX=y',
        'BR2_PACKAGE_DROPBEAR=y', 'BR2_PACKAGE_LIBDRM=y',
        'BR2_PACKAGE_ROCKCHIP_MPP=y', 'BR2_PACKAGE_ROCKCHIP_MPP_TESTS=y',
    ):
        require(setting in config, f"missing config: {setting}")
    for package in ('WESTON', 'CHROMIUM_WAYLAND', 'QT5', 'WPA_SUPPLICANT',
                    'DHCPCD', 'NETWORK_MANAGER', 'PULSEAUDIO', 'ANDROID_ADBD'):
        require(f"BR2_PACKAGE_{package}=y" not in config, f"unexpected package: {package}")
    for fs in ('CPIO', 'SQUASHFS', 'TAR'):
        require(f"BR2_TARGET_ROOTFS_{fs}=y" not in config, f"unexpected rootfs: {fs}")

    names = ('MiniLoaderAll.bin', 'uboot.img', 'boot.img', 'recovery.img',
             'rootfs.img', 'oem.img', 'userdata.img', 'misc.img', 'update.img')
    sizes = {}
    for name in names:
        path = firmware / name
        require(path.is_file() and path.stat().st_size > 0, f"missing firmware: {path}")
        sizes[name] = path.stat().st_size
    for name in ('boot.img', 'recovery.img'):
        with (firmware / name).open('rb') as src:
            require(src.read(4) == b'\xd0\x0d\xfe\xed', f"not a FIT image: {name}")
    with (firmware / 'update.img').open('rb') as src:
        require(src.read(4) == b'RKFW', 'not a Rockchip update package')
    require('TYPE: GPT' in (firmware / 'parameter.txt').read_text(), 'missing GPT layout')

    image = firmware / 'rootfs.img'

    def debugfs(command):
        result = subprocess.run(['debugfs', '-R', command, str(image)],
                                text=True, capture_output=True, check=True)
        require('File not found' not in result.stderr, result.stderr.strip())
        return result.stdout

    interfaces = debugfs('cat /etc/network/interfaces')
    require('address 192.168.77.3' in interfaces, 'wrong board IP in ext4 image')
    require('netmask 255.255.255.0' in interfaces, 'wrong network mask')
    require('iface eth0 inet static' in interfaces, 'eth0 is not static')
    require(not any(line.strip().startswith(('gateway ', 'dns-nameservers '))
                    for line in interfaces.splitlines()), 'unexpected gateway or DNS')
    require(debugfs('cat /etc/hostname').strip() == 'zenith-zero3e', 'wrong hostname')
    require('DROPBEAR_ARGS="-B"' in debugfs('cat /etc/default/dropbear'),
            'development SSH must permit blank-password/none authentication')
    require(debugfs('cat /etc/shadow').startswith('root::'),
            'development root account must have an empty password')
    require('start-stop-daemon' in debugfs('cat /etc/init.d/S50dropbear'),
            'missing SSH startup service')
    require('::respawn:-/bin/sh' in debugfs('cat /etc/inittab'), 'missing serial shell')
    init = debugfs('ls -p /etc/init.d')
    for forbidden in ('weston', 'chromium', 'async-commit', 'usbdevice', 'dhcpcd'):
        require(forbidden not in init, f"unexpected startup service: {forbidden}")
    for binary in ('/usr/sbin/dropbear', '/usr/bin/modetest', '/usr/bin/mpi_dec_test'):
        require('Type: regular' in debugfs(f'stat {binary}'), f"missing binary: {binary}")
    # Verify a binary from the actual filesystem image, not just target/.
    with tempfile.TemporaryDirectory(prefix='zt-rootfs-check-') as tmp:
        binary = Path(tmp) / 'dropbear'
        debugfs(f'dump /usr/sbin/dropbear {binary}')
        header = binary.read_bytes()[:20]
        require(header[:4] == b'\x7fELF' and struct.unpack('<H', header[18:20])[0] == 183,
                'rootfs SSH server is not AArch64 ELF')
    release = (sdk / 'kernel/include/config/kernel.release').read_text().strip()
    return {'sdk': str(sdk), 'kernel_release': release, 'board_ip': '192.168.77.3/24',
            'hostname': 'zenith-zero3e', 'ssh_auth': 'none (empty root password, Dropbear -B)',
            'images_bytes': sizes, 'image_inspection': 'PASS', 'board_boot': 'not tested'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk-root', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    report = check(args.sdk_root.resolve())
    result = json.dumps(report, indent=2) + '\n'
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(result)
    print(result, end='')


if __name__ == '__main__':
    main()
