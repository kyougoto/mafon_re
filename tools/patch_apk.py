#!/usr/bin/env python3
"""Assemble the arm64-enabled Mafon APK from the original APK.

The patch is intentionally minimal: every original zip entry is copied
unchanged (byte for byte), the old v1 signature files are dropped, and the
arm64-v8a native libraries are added under lib/arm64-v8a/.  Optionally the
binary AndroidManifest.xml can be patched in place to raise
android:targetSdkVersion (needed because Android 14 refuses to install apps
that target an API level below 23).

Typical use::

    python patch_apk.py --src mafon-original.apk \
                        --libs dist/lib/arm64-v8a \
                        --out build/mafon-1.0-arm64-unsigned.apk

    python patch_apk.py --src mafon-original.apk \
                        --libs dist/lib/arm64-v8a \
                        --target-sdk 25 \
                        --out build/mafon-1.0-arm64-sdk25-unsigned.apk

Then zipalign + apksigner (see tools/sign_apk.ps1).
"""

import argparse
import os
import struct
import sys
import zipfile

# META-INF entries that carry the old signature and must be removed before
# re-signing.  apksigner regenerates them.
SIGNATURE_NAMES = {
    "MANIFEST.MF",
    "CERT.SF",
    "CERT.RSA",
    "CERT.DSA",
    "CERT.EC",
}
SIGNATURE_SUFFIXES = (".SF", ".RSA", ".DSA", ".EC")

LIBS = ("libmpg.so", "libsonic.so")
ABI = "arm64-v8a"

# android:targetSdkVersion resource id
ATTR_TARGET_SDK = 0x01010270
CHUNK_XML = 0x0003
CHUNK_STRING_POOL = 0x0001
CHUNK_RESOURCE_MAP = 0x0180
CHUNK_START_TAG = 0x0102
TYPE_INT_DEC = 0x10
TYPE_INT_HEX = 0x11


def is_signature_entry(name: str) -> bool:
    base = name.rsplit("/", 1)[-1].upper()
    if base in SIGNATURE_NAMES:
        return True
    return base.endswith(SIGNATURE_SUFFIXES)


def collect_libs(libs_dir: str):
    found = {}
    for lib in LIBS:
        path = os.path.join(libs_dir, lib)
        if not os.path.isfile(path):
            sys.exit("missing native library: %s" % path)
        found[lib] = path
    return found


def patch_axml_target_sdk(data: bytes, new_target: int) -> bytes:
    """Return AndroidManifest.xml bytes with android:targetSdkVersion replaced.

    The manifest is a compiled Android binary XML document.  Only the 32-bit
    data payload of the targetSdkVersion attribute is rewritten; everything
    else (including all chunk offsets) stays byte-identical.
    """
    buf = bytearray(data)
    if len(buf) < 8:
        raise ValueError("manifest too short")
    magic, = struct.unpack_from("<H", buf, 0)
    if magic != CHUNK_XML:
        raise ValueError("not a compiled AndroidManifest.xml (magic=0x%04x)" % magic)

    # Walk the top-level chunks.
    pos = 8
    resource_map = None
    patched = 0
    while pos + 8 <= len(buf):
        chunk_type, header_size, chunk_size = struct.unpack_from("<HHI", buf, pos)
        if chunk_size < 8:
            break
        if chunk_type == CHUNK_RESOURCE_MAP:
            count = (chunk_size - header_size) // 4
            resource_map = list(struct.unpack_from("<%dI" % count, buf, pos + header_size))
        elif chunk_type == CHUNK_START_TAG:
            attrs_start, attr_size, attr_count = struct.unpack_from("<HHH", buf, pos + 16 + 8)
            for i in range(attr_count):
                off = pos + 16 + attrs_start + i * attr_size
                _ns, name_idx, _raw = struct.unpack_from("<iii", buf, off)
                size, _res0, data_type, value = struct.unpack_from("<HBBI", buf, off + 12)
                if resource_map is not None and 0 <= name_idx < len(resource_map):
                    attr_id = resource_map[name_idx]
                else:
                    attr_id = None
                if attr_id == ATTR_TARGET_SDK and data_type in (TYPE_INT_DEC, TYPE_INT_HEX):
                    struct.pack_into("<I", buf, off + 16, new_target)
                    patched += 1
        pos += chunk_size

    if patched != 1:
        raise ValueError("expected exactly one targetSdkVersion attribute, patched %d" % patched)
    return bytes(buf)


def patch(src_apk: str, libs_dir: str, out_apk: str, target_sdk=None) -> None:
    libs = collect_libs(libs_dir)

    with zipfile.ZipFile(src_apk, "r") as zin, \
            zipfile.ZipFile(out_apk, "w") as zout:
        names = set()
        for item in zin.infolist():
            names.add(item.filename)
            if is_signature_entry(item.filename):
                continue
            data = zin.read(item.filename)
            if item.filename == "AndroidManifest.xml" and target_sdk is not None:
                data = patch_axml_target_sdk(data, int(target_sdk))
            target = zipfile.ZipInfo(item.filename, date_time=item.date_time)
            target.compress_type = item.compress_type
            target.external_attr = item.external_attr
            target.internal_attr = item.internal_attr
            target.create_system = item.create_system
            target.flag_bits = item.flag_bits & ~0x08  # deterministic, no data descriptor
            zout.writestr(target, data)

        for lib, path in libs.items():
            entry = "lib/%s/%s" % (ABI, lib)
            if entry in names:
                continue
            with open(path, "rb") as fh:
                data = fh.read()
            info = zipfile.ZipInfo(entry, date_time=(2024, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o100644 << 16)
            info.create_system = 3
            zout.writestr(info, data)

    print("wrote %s (%.2f MiB)" % (out_apk, os.path.getsize(out_apk) / 1048576.0))
    print("added ABI: %s -> %s" % (ABI, ", ".join(sorted(libs))))
    if target_sdk is not None:
        print("patched android:targetSdkVersion -> %d" % int(target_sdk))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True, help="original (unmodified) APK")
    ap.add_argument("--libs", required=True, help="directory containing libmpg.so and libsonic.so")
    ap.add_argument("--out", required=True, help="output unsigned APK path")
    ap.add_argument("--target-sdk", type=int, default=None,
                    help="optionally rewrite android:targetSdkVersion in the binary manifest")
    args = ap.parse_args()

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    patch(args.src, args.libs, args.out, args.target_sdk)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
