#!/usr/bin/env python3
"""gpu_burn.py — 不需要標頭檔：用 ctypes 呼叫 libEGL/libGLESv2，EGL surfaceless + FBO，
跑一個重度 fragment shader，量 GPU 的 FP32 吞吐量。用法：python3 gpu_burn.py [秒數=10] [每像素迴圈=512]"""
import ctypes as C, sys, time
egl = C.CDLL('libEGL.so.1'); gl = C.CDLL('libGLESv2.so.2')
P = C.c_void_p
egl.eglGetProcAddress.restype = P
egl.eglGetPlatformDisplay.restype = P; egl.eglGetPlatformDisplay.argtypes = [C.c_uint, P, P]
egl.eglCreateContext.restype = P; egl.eglCreateContext.argtypes = [P, P, P, P]
egl.eglMakeCurrent.argtypes = [P, P, P, P]; egl.eglInitialize.argtypes = [P, P, P]
egl.eglChooseConfig.argtypes = [P, P, P, C.c_int, P]
gl.glGetString.restype = C.c_char_p
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 10
ITER = int(sys.argv[2]) if len(sys.argv) > 2 else 512
# 直接指定 panthor 的 render node：GBM 平台 + 無 config 的 surfaceless context
import os
NODE = os.environ.get('GPU_NODE', '/dev/dri/renderD130')
gbm = C.CDLL('libgbm.so.1'); gbm.gbm_create_device.restype = P
dev = gbm.gbm_create_device(os.open(NODE, os.O_RDWR)); assert dev, 'gbm'
dpy = egl.eglGetPlatformDisplay(0x31D7, dev, None)            # EGL_PLATFORM_GBM_KHR
assert egl.eglInitialize(dpy, None, None), 'eglInitialize'
egl.eglBindAPI(0x30A0)                                        # EGL_OPENGL_ES_API
ca = (C.c_int * 3)(0x3098, 3, 0x3038)                         # CONTEXT_MAJOR_VERSION=3
ctx = egl.eglCreateContext(dpy, None, None, ca); assert ctx, 'ctx'  # EGL_NO_CONFIG_KHR
assert egl.eglMakeCurrent(dpy, None, None, ctx)
print('GL_RENDERER =', gl.glGetString(0x1F01).decode(), '| GL_VERSION =', gl.glGetString(0x1F02).decode())
def shader(kind, src):
    s = gl.glCreateShader(kind); b = C.c_char_p(src.encode())
    gl.glShaderSource(s, 1, C.byref(b), None); gl.glCompileShader(s)
    ok = C.c_int(); gl.glGetShaderiv(s, 0x8B81, C.byref(ok))
    if not ok.value:
        log = C.create_string_buffer(4096); gl.glGetShaderInfoLog(s, 4096, None, log); sys.exit(log.value.decode())
    return s
vs = shader(0x8B31, '''#version 300 es
void main(){ vec2 p = vec2((gl_VertexID<<1)&2, gl_VertexID&2); gl_Position = vec4(p*2.0-1.0,0,1); }''')
fs = shader(0x8B30, '''#version 300 es
precision highp float; out vec4 o; uniform float k;
void main(){ vec4 a = vec4(gl_FragCoord.xyxy)*1e-4, b = a+0.1, c = a+0.2, d = a+0.3;
  for (int i = 0; i < %d; i++) { a = a*k + 0.5; b = b*k + 0.5; c = c*k + 0.5; d = d*k + 0.5; }
  o = a+b+c+d; }''' % ITER)
prog = gl.glCreateProgram(); gl.glAttachShader(prog, vs); gl.glAttachShader(prog, fs); gl.glLinkProgram(prog)
gl.glUseProgram(prog); gl.glUniform1f.argtypes = [C.c_int, C.c_float]
gl.glUniform1f(gl.glGetUniformLocation(prog, b'k'), 0.999)
W = H = 1024
tex = C.c_uint(); gl.glGenTextures(1, C.byref(tex)); gl.glBindTexture(0x0DE1, tex)
gl.glTexStorage2D(0x0DE1, 1, 0x8058, W, H)                    # GL_RGBA8
fbo = C.c_uint(); gl.glGenFramebuffers(1, C.byref(fbo)); gl.glBindFramebuffer(0x8D40, fbo)
gl.glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, tex, 0)
assert gl.glCheckFramebufferStatus(0x8D40) == 0x8CD5, 'FBO incomplete'
vao = C.c_uint(); gl.glGenVertexArrays(1, C.byref(vao)); gl.glBindVertexArray(vao)
gl.glViewport(0, 0, W, H)
flop_per_draw = W * H * ITER * 4 * 4 * 2                      # 4 個 vec4 × 4 lanes × FMA(2)
DPF = int(os.environ.get('DPF', '4'))                         # 每次 glFinish 前畫幾次
if os.environ.get('BLEND') == '1':                            # 開混色 → 後畫的不能蓋掉先畫的
    gl.glEnable(0x0BE2); gl.glBlendFunc(1, 1)                 # GL_BLEND, ONE, ONE
print('DPF=%d BLEND=%s' % (DPF, os.environ.get('BLEND', '0')))
draws, t0 = 0, time.monotonic()
while time.monotonic() - t0 < SECS:
    for _ in range(DPF): gl.glDrawArrays(4, 0, 3)
    gl.glFinish(); draws += DPF
dt = time.monotonic() - t0
print('draws=%d time=%.2fs  %.1f GFLOPS (FP32, 估算)' % (draws, dt, draws * flop_per_draw / dt / 1e9))
