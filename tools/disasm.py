#!/usr/bin/env python3
"""Development helper: disassemble the linked guest image. Usage: dis.py ADDRESS [BYTES]"""
import sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
pe = pefile.PE('out/linked.exe')
base = pe.OPTIONAL_HEADER.ImageBase
a = int(sys.argv[1], 16); n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x80
for i in Cs(CS_ARCH_X86, CS_MODE_32).disasm(pe.get_data(a - base, n), a):
    print(f'{i.address:08x} {i.mnemonic} {i.op_str}')
