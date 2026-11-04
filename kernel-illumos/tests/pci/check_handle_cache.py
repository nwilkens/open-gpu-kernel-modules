#!/usr/bin/env python3
"""Check the live NVIDIA PCI handle cache with MDB on an illumos host."""

import argparse
import re
import shlex
import subprocess
import sys


def mdb(command, host=None):
    argv = ["mdb", "-k", "-e", command]
    if host is not None:
        argv = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
                "-o", "HostKeyAlias=saruman.local", host,
                "mdb -k -e " + shlex.quote(command)]
    return subprocess.run(
        argv, check=True, capture_output=True,
        text=True).stdout


def parse_words(output):
    words = re.findall(r"\b[0-9a-fA-F]{1,16}\b", output.split(":", 1)[1])
    if len(words) != 5:
        raise ValueError(f"expected five MDB words: {output!r}")
    return tuple(int(word, 16) for word in words)


def inspect(run, gpu_bdf=None):
    addresses = re.findall(r"(?m)^0x[0-9a-fA-F]+$", run("nv_pci_handles::walk list"))
    if not addresses:
        raise ValueError("no NVIDIA PCI handles found")
    seen = {}
    for address in addresses:
        words = parse_words(run(f"{address}/5K"))
        dip, cfg, bdf = words[2], words[3], words[4] & 0xffff
        if bdf in seen:
            raise ValueError(f"duplicate BDF {bdf:04x}: {seen[bdf]}, {address}")
        seen[bdf] = address
        if bdf == gpu_bdf and (dip == 0 or cfg == 0):
            raise ValueError(f"GPU {bdf:04x} lacks dip/cfg: {address}")
    if gpu_bdf is not None and gpu_bdf not in seen:
        raise ValueError(f"GPU BDF {gpu_bdf:04x} missing")
    return len(addresses)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gpu-bdf", type=lambda value: int(value, 16),
                        help="attached GPU BDF as hexadecimal, e.g. 0100")
    parser.add_argument("--ssh", help="run MDB through SSH on this host")
    args = parser.parse_args()
    try:
        count = inspect(lambda command: mdb(command, args.ssh), args.gpu_bdf)
    except (ValueError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        return 1
    print(f"{count} handles; unique BDFs; attached GPU fields present if requested")
    return 0


if __name__ == "__main__":
    sys.exit(main())
