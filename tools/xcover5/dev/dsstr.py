#!/usr/bin/env python3
"""dsstr.py ADDR [HEXLEN] : the C string (and first bytes) at an unslid shared-cache address.
A CFString constant (assertion text, NSString literal) is 32 bytes: isa, flags, char* at +16, length at +24 --
read it, then read the char* it holds."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dsc
addr = int(sys.argv[1], 16)
n = int(sys.argv[2], 16) if len(sys.argv) > 2 else 200
b = dsc.read(addr, n)
if b is None:
    print('unmapped')
else:
    print(repr(b.split(b'\0')[0]))
    print(b[:64].hex())
