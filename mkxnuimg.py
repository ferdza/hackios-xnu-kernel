#!/usr/bin/env python3
"""
Pack a device's UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image for `fastboot boot`.

  kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB

The boot shim copies the FD to its base and the payload to PAYLOAD_BASE.

Which device: -d/--device, else $DEVICE, else poco-x5-pro. The POCO (redwood) is the built-in
default and its image is byte-identical to before. Any other device supplies
devices/<device>/uefi/uefi.conf with the UEFI package stem and the FD base/size (from that device's
Mu-Silicium *.toml [uefi_fd]); the OnePlus 6T's FD differs from the POCO's on BOTH base and size, so
it MUST come from there, not from the redwood default.

The UEFI pieces are taken from a Mu-Silicium tree built here (Mu-Silicium/, see
devices/<device>/uefi/README.md) or, when there is none, from devices/<device>/prebuilt/.

usage: mkxnuimg.py [-d DEVICE] [-p payload.bin] [-o out.img]
"""
import argparse, gzip, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(HERE, 'Mu-Silicium')
PAYLOAD_BASE, PAYLOAD_MAX = 0xF0000000, 0x04000000

ap = argparse.ArgumentParser()
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output')
ap.add_argument('--mode', default='RELEASE')
ap.add_argument('-d', '--device', default=os.environ.get('DEVICE', 'poco-x5-pro'),
                help='device dir under devices/ (default: $DEVICE or poco-x5-pro)')
args = ap.parse_args()

# Per-device UEFI packaging values. redwood (the POCO) is the built-in default so nothing changes for
# it; every other device must ship devices/<device>/uefi/uefi.conf (see that file for the format).
conf = os.path.join(HERE, 'devices', args.device, 'uefi', 'uefi.conf')
if os.path.isfile(conf):
    cfg = {}
    for raw in open(conf):
        line = raw.split('#', 1)[0].strip()
        if '=' in line:
            k, v = line.split('=', 1); cfg[k.strip()] = v.strip()
    stem = cfg['stem']
    FD_BASE, FD_SIZE = int(cfg['fd_base'], 0), int(cfg['fd_size'], 0)
elif args.device == 'poco-x5-pro':
    stem, FD_BASE, FD_SIZE = 'redwood', 0x9FD00000, 0x200000
else:
    sys.exit(f'mkxnuimg.py: no devices/{args.device}/uefi/uefi.conf (needed for any non-POCO device)')
PREBUILT = os.path.join(HERE, 'devices', args.device, 'prebuilt')
if args.output is None:
    args.output = os.path.join(HERE, f'xnu-{stem}.img')

fd_path = os.path.join(MU, f'Build/{stem}Pkg/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')
if os.path.isfile(fd_path):
    shim_dir = os.path.join(MU, 'BootShim')
    for f in ('BootShim.elf', 'BootShim.bin'):
        try: os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError: pass
    subprocess.check_call(['make', 'REQUIRES_KERNEL_HEADER=1', f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
                           f'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}'],
                          cwd=shim_dir, stdout=subprocess.DEVNULL)
    parts = {'shim': os.path.join(shim_dir, 'BootShim.bin'), 'fd': fd_path,
             'dtb': os.path.join(MU, f'Resources/DTBs/{stem}.dtb'), 'ramdisk': os.path.join(MU, 'Resources/ramdisk'),
             'mkbootimg': os.path.join(MU, 'Resources/Scripts/mkbootimg.py')}
elif os.path.isfile(os.path.join(PREBUILT, 'SILICIUM_UEFI.fd')):
    parts = {'shim': os.path.join(PREBUILT, 'BootShim.bin'), 'fd': os.path.join(PREBUILT, 'SILICIUM_UEFI.fd'),
             'dtb': os.path.join(PREBUILT, 'device-tree.dtb'), 'ramdisk': os.path.join(PREBUILT, 'ramdisk'),
             'mkbootimg': os.path.join(PREBUILT, 'mkbootimg.py')}
else:
    sys.exit(f'mkxnuimg.py: no built UEFI for {args.device}\n'
             f'  build it:  cd Mu-Silicium && python3 build_uefi.py -d {stem} -r {args.mode}\n'
             f'  (looked for {fd_path} and {PREBUILT}/SILICIUM_UEFI.fd)')
shim = open(parts['shim'], 'rb').read()
fd = open(parts['fd'], 'rb').read()
assert len(fd) == FD_SIZE, hex(len(fd))

blob = shim + fd
if args.payload:
    data = open(args.payload, 'rb').read()
    # the payload file is a complete "XNUP" container (see xnuloader/mkpayload.py)
    assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print(f'payload: {len(data)} bytes, bytesum={sum(data) & 0xffffffff:08x}')

dtb = open(parts['dtb'], 'rb').read()
kernel = args.output + '.kernel~'
open(kernel, 'wb').write(gzip.compress(blob, 6) + dtb)
try:
    subprocess.check_call([sys.executable, parts['mkbootimg'], '--kernel', kernel, '--ramdisk', parts['ramdisk'],
                           '--header_version', '1', '-o', args.output])
finally:
    os.unlink(kernel)
print(f'{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})')
