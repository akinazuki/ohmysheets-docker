#!/usr/bin/env python3
"""
Patch Android ARM64 ELF .so files to run on Linux glibc.

Fixes four incompatibilities:
1. .gnu.version - Android uses LIBC version tag, glibc expects GLIBC_2.x
   -> Zero ALL version entries so the dynamic linker skips version checks
2. DT_VERNEED/DT_VERNEEDNUM - References to Bionic version requirements
   -> Null these entries in .dynamic section
3. Hardcoded paths - Developer paths baked into .rodata
   -> Patch to runtime-appropriate paths
4. .note.android.ident - glibc ld.so refuses Android-noted ELFs
   -> Removed separately via objcopy (must use binutils, not pure Python,
      because removing a section requires relayout)

Usage: python3 patch_elf.py <file.so> [file2.so ...]
"""
import struct
import sys
import os


def patch_elf(path):
    with open(path, 'rb') as f:
        data = bytearray(f.read())

    # Verify ELF magic
    if data[:4] != b'\x7fELF':
        print(f"  {path}: not an ELF file, skipping")
        return False

    ei_class = data[4]
    if ei_class != 2:
        print(f"  {path}: not 64-bit ELF, skipping")
        return False

    # Parse ELF64 header
    e_shoff = struct.unpack_from('<Q', data, 40)[0]
    e_shentsize = struct.unpack_from('<H', data, 58)[0]
    e_shnum = struct.unpack_from('<H', data, 60)[0]
    e_shstrndx = struct.unpack_from('<H', data, 62)[0]

    # Section header string table
    shstr_off = struct.unpack_from('<Q', data, e_shoff + e_shstrndx * e_shentsize + 24)[0]

    def sec_name(sh_name):
        end = data.index(0, shstr_off + sh_name)
        return data[shstr_off + sh_name:end].decode()

    patches = []

    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name_idx = struct.unpack_from('<I', data, off)[0]
        name = sec_name(sh_name_idx)
        sh_offset = struct.unpack_from('<Q', data, off + 24)[0]
        sh_size = struct.unpack_from('<Q', data, off + 32)[0]

        # 1. Zero ALL .gnu.version entries (SHT_GNU_versym)
        #    Must zero everything (including v=1) to fully bypass version checks
        if name == '.gnu.version':
            count = 0
            for j in range(sh_offset, sh_offset + sh_size, 2):
                v = struct.unpack_from('<H', data, j)[0]
                if v != 0:
                    struct.pack_into('<H', data, j, 0)
                    count += 1
            if count:
                patches.append(f".gnu.version: zeroed {count} entries")

        # 2. Null DT_VERNEED/DT_VERNEEDNUM in .dynamic
        if name == '.dynamic':
            for j in range(sh_offset, sh_offset + sh_size, 16):
                tag = struct.unpack_from('<Q', data, j)[0]
                if tag == 0:
                    break
                # DT_VERNEED (0x6ffffffe) or DT_VERNEEDNUM (0x6fffffff)
                if tag in (0x6ffffffe, 0x6fffffff):
                    struct.pack_into('<Q', data, j, 0)
                    struct.pack_into('<Q', data, j + 8, 0)
                    patches.append(f"DT tag 0x{tag:x} nulled")

    # 3. Patch hardcoded developer paths in .rodata
    path_patches = [
        (b'/Users/zemsti/Documents/Projekty/MusiReader/tests/images/correlation/templates/',
         b'/app/assets/templates/'),
        (b'/Users/zemsti/Documents/Projekty/MusiReader/output/_log/',
         b'/app/output/'),
    ]
    for old_path, new_path in path_patches:
        idx = data.find(old_path)
        if idx >= 0:
            padded = new_path + b'\x00' * (len(old_path) - len(new_path))
            data[idx:idx + len(old_path)] = padded
            patches.append(f"path patched at 0x{idx:x}: {old_path.decode()} -> {new_path.decode()}")

    if patches:
        with open(path, 'wb') as f:
            f.write(data)
        print(f"  {os.path.basename(path)}: {'; '.join(patches)}")
        return True
    else:
        print(f"  {os.path.basename(path)}: no patches needed")
        return False


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <file.so> [file2.so ...]")
        sys.exit(1)

    for path in sys.argv[1:]:
        patch_elf(path)
