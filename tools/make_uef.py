#!/usr/bin/env python3
"""
make_uef.py - Generate a minimal UEF tape file for BBC Micro testing.

Encodes a tiny BBC BASIC II program:
   10 PRINT "HELLO FROM TAPE"
   20 END

BBC BASIC tokenised format and BBC tape block format are both implemented here.

Usage:
    python3 make_uef.py hello.uef
"""

import struct
import sys

# ---------------------------------------------------------------------------
# BBC BASIC II tokenisation
# Tokens for: 10 PRINT "HELLO FROM TAPE"\r 20 END\r
# Line format: <hi_ln> <lo_ln> <line_len> <bytes...> <0x0D>
# ---------------------------------------------------------------------------
PRINT_TOKEN = 0xF1
END_TOKEN   = 0xE0

def bbc_basic_line(line_num: int, tokens: bytes) -> bytes:
    # Line = HI(linenum) LO(linenum) len tokens 0x0D
    # len = 4 + len(tokens)  (hi + lo + len_byte + tokens + 0x0D)
    body = tokens + b'\x0D'
    length = 4 + len(body)      # hi + lo + len + body
    return bytes([line_num >> 8, line_num & 0xFF, length]) + body

line10 = bbc_basic_line(10, bytes([PRINT_TOKEN]) + b' "HELLO FROM TAPE"')
line20 = bbc_basic_line(20, bytes([END_TOKEN]))
# Program terminator: FF FF 00
basic_prog = line10 + line20 + b'\xFF\xFF\x00'

LOAD_ADDR = 0x0E00   # standard BASIC load address
EXEC_ADDR = 0x8000   # BASIC entry point (MOS calls this after CHAIN)

# ---------------------------------------------------------------------------
# CRC-16 (BBC tape uses CCITT CRC-16 with polynomial 0x1021, init 0x0000)
# ---------------------------------------------------------------------------
def crc16(data: bytes) -> int:
    crc = 0x0000
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = (crc << 1) ^ 0x1021
            else:
                crc <<= 1
            crc &= 0xFFFF
    return crc

# ---------------------------------------------------------------------------
# Build BBC tape block
# Block layout (after the 0x2A sync byte):
#   10 bytes filename (null-padded)
#    4 bytes load address (LE)
#    4 bytes exec address (LE)
#    2 bytes block number (LE)
#    2 bytes block length (LE)
#    1 byte  flags  (0x80 = last block)
#    4 bytes next file address (LE, usually 0)
#    2 bytes header CRC (over all header bytes above, LE)
#  [N bytes data]
#    2 bytes data CRC (LE)
# ---------------------------------------------------------------------------
def make_tape_block(filename: str, load: int, exec_: int,
                    block_num: int, data: bytes, last: bool) -> bytes:
    fname = filename[:10].encode('ascii').ljust(10, b'\x00')
    flags = 0x80 if last else 0x00
    # Header (before CRC):
    hdr = (fname
           + struct.pack('<I', load)
           + struct.pack('<I', exec_)
           + struct.pack('<H', block_num)
           + struct.pack('<H', len(data))
           + bytes([flags])
           + struct.pack('<I', 0))   # next file addr
    hcrc = crc16(hdr)
    dcrc = crc16(data) if data else crc16(b'')
    return b'\x2A' + hdr + struct.pack('<H', hcrc) + data + struct.pack('<H', dcrc)

# ---------------------------------------------------------------------------
# Split BASIC program into 256-byte tape blocks
# ---------------------------------------------------------------------------
BLOCK_SIZE = 256

def make_all_blocks(filename: str, load: int, exec_: int, data: bytes) -> list:
    blocks = []
    offset = 0
    block_num = 0
    while offset < len(data):
        chunk = data[offset:offset + BLOCK_SIZE]
        last  = (offset + BLOCK_SIZE >= len(data))
        blocks.append(make_tape_block(filename, load, exec_,
                                      block_num, chunk, last))
        offset    += len(chunk)
        block_num += 1
    if not blocks:
        # Empty file: one empty last block
        blocks.append(make_tape_block(filename, load, exec_, 0, b'', True))
    return blocks

# ---------------------------------------------------------------------------
# Assemble UEF file
# Magic: "UEF File!\x00" + version minor(0x00) + major(0x0A)
# Chunk: 2-byte ID (LE) + 4-byte len (LE) + data
# ---------------------------------------------------------------------------
def make_uef(blocks: list) -> bytes:
    out = b'UEF File!\x00' + bytes([0x00, 0x0A])  # version 10.0
    for blk in blocks:
        chunk_id  = struct.pack('<H', 0x0100)
        chunk_len = struct.pack('<I', len(blk))
        out += chunk_id + chunk_len + blk
    return out

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
if __name__ == '__main__':
    outpath = sys.argv[1] if len(sys.argv) > 1 else 'hello.uef'
    blocks  = make_all_blocks('HELLO', LOAD_ADDR, EXEC_ADDR, basic_prog)
    uef     = make_uef(blocks)
    with open(outpath, 'wb') as f:
        f.write(uef)
    print(f"Written {outpath}: {len(blocks)} block(s), {len(basic_prog)} bytes of BASIC")
    print(f"Load=0x{LOAD_ADDR:04X}  Exec=0x{EXEC_ADDR:04X}")
    print(f"BASIC program bytes: {basic_prog.hex()}")
