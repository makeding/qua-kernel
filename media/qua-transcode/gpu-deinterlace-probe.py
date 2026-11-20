#!/usr/bin/env python3
"""Measure one-field NV12 GPU interpolation including host upload/readback."""
import ctypes as c
import ctypes.util
import os
import time

os.environ.setdefault('EGL_PLATFORM', 'surfaceless')
def lib(name):
    path = ctypes.util.find_library(name)
    if not path:
        raise RuntimeError('missing library ' + name)
    return c.CDLL(path)
def fn(libobj, name, result, *args):
    f = getattr(libobj, name); f.restype = result; f.argtypes = args; return f

egl, gl = lib('EGL'), lib('GLESv2')
P, I, U, S = c.c_void_p, c.c_int, c.c_uint, c.c_ssize_t
display = fn(egl, 'eglGetDisplay', P, P)(None)
init = fn(egl, 'eglInitialize', U, P, c.POINTER(I), c.POINTER(I))
choose = fn(egl, 'eglChooseConfig', U, P, c.POINTER(I), c.POINTER(P), I, c.POINTER(I))
bind = fn(egl, 'eglBindAPI', U, U)
context_fn = fn(egl, 'eglCreateContext', P, P, P, P, c.POINTER(I))
surface_fn = fn(egl, 'eglCreatePbufferSurface', P, P, P, c.POINTER(I))
current = fn(egl, 'eglMakeCurrent', U, P, P, P, P)
destroy_surface = fn(egl, 'eglDestroySurface', U, P, P)
destroy_context = fn(egl, 'eglDestroyContext', U, P, P)
terminate = fn(egl, 'eglTerminate', U, P)
get_string = fn(gl, 'glGetString', c.c_char_p, U)
shader = fn(gl, 'glCreateShader', U, U)
shader_source = fn(gl, 'glShaderSource', None, U, I, c.POINTER(c.c_char_p), c.POINTER(I))
compile_shader = fn(gl, 'glCompileShader', None, U)
get_shader = fn(gl, 'glGetShaderiv', None, U, U, c.POINTER(I))
shader_log = fn(gl, 'glGetShaderInfoLog', None, U, I, c.POINTER(I), c.c_char_p)
program_fn = fn(gl, 'glCreateProgram', U)
attach = fn(gl, 'glAttachShader', None, U, U)
link = fn(gl, 'glLinkProgram', None, U)
get_program = fn(gl, 'glGetProgramiv', None, U, U, c.POINTER(I))
program_log = fn(gl, 'glGetProgramInfoLog', None, U, I, c.POINTER(I), c.c_char_p)
use = fn(gl, 'glUseProgram', None, U)
gen_buffers = fn(gl, 'glGenBuffers', None, I, c.POINTER(U))
bind_buffer = fn(gl, 'glBindBuffer', None, U, U)
buffer_data = fn(gl, 'glBufferData', None, U, S, P, U)
buffer_sub = fn(gl, 'glBufferSubData', None, U, S, S, P)
bind_base = fn(gl, 'glBindBufferBase', None, U, U, U)
dispatch = fn(gl, 'glDispatchCompute', None, U, U, U)
barrier = fn(gl, 'glMemoryBarrier', None, U)
map_buffer = fn(gl, 'glMapBufferRange', P, U, S, S, U)
unmap = fn(gl, 'glUnmapBuffer', U, U)
error = fn(gl, 'glGetError', U)

src = b'''#version 310 es
layout(local_size_x=64) in;
layout(std430,binding=0) readonly buffer Input { uint src[]; };
layout(std430,binding=1) writeonly buffer Output { uint dst[]; };
void main() {
    uint i=gl_GlobalInvocationID.x;
    const uint stride=360u, rows=1620u, yrows=1080u;
    if (i>=stride*rows) return;
    uint y=i/stride, local_y=y<yrows ? y : y-yrows;
    uint count=y<yrows ? yrows : rows-yrows;
    if ((local_y&1u)==0u) { dst[i]=src[i]; return; }
    uint a=src[i-stride];
    uint b=local_y+1u<count ? src[i+stride] : a;
    dst[i]=(a&b)+(((a^b)&0xfefefefeu)>>1u);
}'''

def require(ok, what):
    if not ok: raise RuntimeError(what)

major, minor = I(), I()
require(display and init(display, c.byref(major), c.byref(minor)), 'eglInitialize')
ctx = surf = None
try:
    attrs = (I * 11)(0x3033, 1, 0x3040, 0x40, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3038)
    cfg, count = P(), I()
    require(choose(display, attrs, c.byref(cfg), 1, c.byref(count)) and count.value, 'eglChooseConfig')
    require(bind(0x30a0), 'eglBindAPI')
    ctx = context_fn(display, cfg, None, (I * 3)(0x3098, 3, 0x3038))
    surf = surface_fn(display, cfg, (I * 5)(0x3057, 8, 0x3056, 8, 0x3038))
    require(ctx and surf and current(display, surf, surf, ctx), 'eglMakeCurrent')
    renderer = (get_string(0x1f01) or b'').decode()
    version = (get_string(0x1f02) or b'').decode()
    print('renderer=%s version=%s' % (renderer, version), flush=True)
    require('panfrost' in renderer.lower(), 'Panfrost required')
    sh = shader(0x91b9); text = c.c_char_p(src)
    shader_source(sh, 1, c.byref(text), None); compile_shader(sh)
    status = I(); get_shader(sh, 0x8b81, c.byref(status))
    if not status.value:
        msg = c.create_string_buffer(8192); shader_log(sh, len(msg), None, msg); raise RuntimeError(msg.value.decode())
    program = program_fn(); attach(program, sh); link(program); get_program(program, 0x8b82, c.byref(status))
    if not status.value:
        msg = c.create_string_buffer(8192); program_log(program, len(msg), None, msg); raise RuntimeError(msg.value.decode())
    use(program)
    width, rows = 1440, 1620
    data = bytearray(width * rows)
    for y in range(rows): data[y * width:(y + 1) * width] = bytes([y % 256 if y % 2 == 0 else 255]) * width
    host_in = (c.c_ubyte * len(data)).from_buffer(data)
    host_out = (c.c_ubyte * len(data))()
    ids = (U * 2)(); gen_buffers(2, ids)
    for i in range(2):
        bind_buffer(0x90d2, ids[i]); buffer_data(0x90d2, len(data), None, 0x88e8); bind_base(0x90d2, i, ids[i])
    require(error() == 0, 'buffer allocation')
    start = time.monotonic(); frames = 60
    for _ in range(frames):
        bind_buffer(0x90d2, ids[0]); buffer_sub(0x90d2, 0, len(data), host_in)
        dispatch((width * rows // 4 + 63) // 64, 1, 1); barrier(0x2000 | 0x0200)
        bind_buffer(0x90d2, ids[1]); mapped = map_buffer(0x90d2, 0, len(data), 1)
        require(mapped, 'glMapBufferRange'); c.memmove(host_out, mapped, len(data)); require(unmap(0x90d2), 'glUnmapBuffer')
    elapsed = time.monotonic() - start
    for y in (0, 1, 2, 3, 1078, 1079, 1080, 1081, 1618, 1619):
        local = y if y < 1080 else y - 1080; count = 1080 if y < 1080 else 540
        expected = data[y*width] if local % 2 == 0 else (data[(y-1)*width] + data[(y+1 if local+1<count else y-1)*width]) // 2
        require(host_out[y*width] == expected, 'incorrect output row %d' % y)
    require(error() == 0, 'GL error')
    print('GPU_BOB frames=%d wall_seconds=%.3f fps=%.2f copy_in_and_out=yes check=passed' % (frames, elapsed, frames/elapsed), flush=True)
finally:
    current(display, None, None, None)
    if surf: destroy_surface(display, surf)
    if ctx: destroy_context(display, ctx)
    terminate(display)
