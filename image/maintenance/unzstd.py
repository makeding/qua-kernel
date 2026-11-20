#!/usr/bin/env python3
"""Stream a zstd archive to stdout using the image's libzstd."""
import ctypes as c
import hashlib
import sys
from contextlib import nullcontext

class In(c.Structure):
    _fields_ = [('src', c.c_void_p), ('size', c.c_size_t), ('pos', c.c_size_t)]
class Out(c.Structure):
    _fields_ = [('dst', c.c_void_p), ('size', c.c_size_t), ('pos', c.c_size_t)]
lib = c.CDLL('libzstd.so.1')
lib.ZSTD_createDStream.restype = c.c_void_p
lib.ZSTD_initDStream.argtypes = [c.c_void_p]
lib.ZSTD_initDStream.restype = c.c_size_t
lib.ZSTD_decompressStream.argtypes = [c.c_void_p, c.POINTER(Out), c.POINTER(In)]
lib.ZSTD_decompressStream.restype = c.c_size_t
lib.ZSTD_isError.argtypes = [c.c_size_t]
lib.ZSTD_isError.restype = c.c_uint
lib.ZSTD_freeDStream.argtypes = [c.c_void_p]
stream = lib.ZSTD_createDStream()
if not stream: raise RuntimeError('cannot allocate zstd stream')
try:
    result = lib.ZSTD_initDStream(stream)
    if lib.ZSTD_isError(result): raise RuntimeError('cannot initialize zstd')
    outbuf = c.create_string_buffer(1024 * 1024)
    digest = hashlib.sha256()
    input_stream = nullcontext(sys.stdin.buffer) if sys.argv[1] == '-' else open(sys.argv[1], 'rb')
    with input_stream as source:
        while data := source.read(1024 * 1024):
            digest.update(data)
            buf = c.create_string_buffer(data)
            inp = In(c.addressof(buf), len(data), 0)
            while inp.pos < inp.size:
                out = Out(c.addressof(outbuf), len(outbuf), 0)
                result = lib.ZSTD_decompressStream(stream, c.byref(out), c.byref(inp))
                if lib.ZSTD_isError(result): raise RuntimeError('invalid zstd stream')
                sys.stdout.buffer.write(outbuf.raw[:out.pos])
    if result != 0: raise RuntimeError('truncated zstd stream')
    if len(sys.argv) > 2 and digest.hexdigest() != sys.argv[2]:
        raise RuntimeError('rootfs SHA-256 mismatch')
finally:
    lib.ZSTD_freeDStream(stream)
