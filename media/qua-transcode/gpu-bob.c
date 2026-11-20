/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Progressive NV12 output from one interlaced field using Panfrost GLES 3.1. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static EGLDisplay display;
static EGLContext context;
static EGLSurface surface;
static GLuint buffers[2], program;
static size_t frame_bytes;
static unsigned words;
static int mapped_upload;
static int dmabuf_mode;
static unsigned image_width, image_rows;
static EGLImageKHR images[64][2];
static GLuint textures[64][2];
static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC image_storage;

static const char shader_source[] =
    "#version 310 es\n"
    "layout(local_size_x=64) in;\n"
    "layout(std430,binding=0) readonly buffer A { uint src[]; };\n"
    "layout(std430,binding=1) writeonly buffer B { uint dst[]; };\n"
    "uniform uint stride_words; uniform uint luma_slice;\n"
    "uniform uint active_height; uniform uint field; uniform uint total_words;\n"
    "void main() { uint i=gl_GlobalInvocationID.x; if(i>=total_words)return;\n"
    "uint y=i/stride_words; bool chroma=y>=luma_slice;\n"
    "uint line=chroma ? y-luma_slice : y; uint count=chroma ? active_height/2u : active_height;\n"
    "if(line>=count || (line&1u)==field){dst[i]=src[i];return;}\n"
    "uint a=src[i+(line==0u ? stride_words : -stride_words)];\n"
    "uint b=src[i+(line+1u>=count ? -stride_words : stride_words)];\n"
    "dst[i]=(a&b)+(((a^b)&0xfefefefeu)>>1u); }\n";

static const char image_shader_source[] =
    "#version 310 es\n"
    "layout(local_size_x=64) in;\n"
    "layout(rgba8,binding=0) readonly uniform highp image2D src_img;\n"
    "layout(rgba8,binding=1) writeonly uniform highp image2D dst_img;\n"
    "uniform uint stride_words; uniform uint luma_slice;\n"
    "uniform uint active_height; uniform uint field; uniform uint total_words;\n"
    "void main(){uint i=gl_GlobalInvocationID.x;if(i>=total_words)return;\n"
    "uint y=i/stride_words; int x=int(i%stride_words); bool c=y>=luma_slice;\n"
    "uint line=c?y-luma_slice:y;uint count=c?active_height/2u:active_height;\n"
    "ivec2 p=ivec2(x,int(y)); vec4 v;\n"
    "if(line>=count || (line&1u)==field) v=imageLoad(src_img,p);\n"
    "else {int a=line==0u?1:-1;int b=line+1u>=count?-1:1;\n"
    "v=(imageLoad(src_img,p+ivec2(0,a))+imageLoad(src_img,p+ivec2(0,b)))*0.5;}\n"
    "imageStore(dst_img,p,v);}\n";

int qua_gpu_bob_init(unsigned stride, unsigned slice, unsigned height, unsigned field)
{
    EGLint major, minor, count;
    EGLConfig config;
    EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                      EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                      EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLint surf_attrs[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
    GLint ok;
    GLuint shader;
    char log[4096];
    if (!stride || stride % 4 || !height || height % 4 || height > slice || slice % 2 || field > 1)
        return -1;
    setenv("EGL_PLATFORM", "surfaceless", 0);
    mapped_upload = getenv("QUA_GPU_UPLOAD") && !strcmp(getenv("QUA_GPU_UPLOAD"), "map");
    dmabuf_mode = getenv("QUA_GPU_IO") && !strcmp(getenv("QUA_GPU_IO"), "dmabuf");
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) ||
        !eglChooseConfig(display, attrs, &config, 1, &count) || !count ||
        !eglBindAPI(EGL_OPENGL_ES_API)) return -1;
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attrs);
    surface = eglCreatePbufferSurface(display, config, surf_attrs);
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE ||
        !eglMakeCurrent(display, surface, surface, context)) return -1;
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    if (!renderer || !strstr(renderer, "Panfrost")) return -1;
    if (dmabuf_mode) {
        create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        image_storage = (PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC)eglGetProcAddress("glEGLImageTargetTexStorageEXT");
        if (!create_image || !destroy_image || !image_storage) return -1;
    }
    shader = glCreateShader(GL_COMPUTE_SHADER);
    const char *src = dmabuf_mode ? image_shader_source : shader_source;
    glShaderSource(shader, 1, &src, NULL); glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) { glGetShaderInfoLog(shader, sizeof(log), NULL, log); fprintf(stderr, "GPU_BOB shader: %s\n", log); return -1; }
    program = glCreateProgram(); glAttachShader(program, shader); glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok); glDeleteShader(shader);
    if (!ok) { glGetProgramInfoLog(program, sizeof(log), NULL, log); fprintf(stderr, "GPU_BOB link: %s\n", log); return -1; }
    glUseProgram(program);
    frame_bytes = (size_t)stride * slice * 3 / 2;
    words = frame_bytes / 4;
    image_width = stride / 4;
    image_rows = slice * 3 / 2;
    glUniform1ui(glGetUniformLocation(program, "stride_words"), stride / 4);
    glUniform1ui(glGetUniformLocation(program, "luma_slice"), slice);
    glUniform1ui(glGetUniformLocation(program, "active_height"), height);
    glUniform1ui(glGetUniformLocation(program, "field"), field);
    glUniform1ui(glGetUniformLocation(program, "total_words"), words);
    if (!dmabuf_mode) {
        glGenBuffers(2, buffers);
        for (unsigned i = 0; i < 2; i++) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[i]);
            glBufferData(GL_SHADER_STORAGE_BUFFER, frame_bytes, NULL, GL_DYNAMIC_COPY);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, buffers[i]);
        }
    }
    if (glGetError() != GL_NO_ERROR) return -1;
    fprintf(stderr, "GPU_BOB renderer=%s stride=%u slice=%u height=%u field=%u io=%s\n", renderer, stride, slice, height, field, dmabuf_mode ? "dmabuf" : "copy");
    return 0;
}

int qua_gpu_bob_register(unsigned slot, int input_fd, int output_fd)
{
    int fds[] = { input_fd, output_fd };
    if (!dmabuf_mode || slot >= 64 || input_fd < 0 || output_fd < 0) return -1;
    glGenTextures(2, textures[slot]);
    for (unsigned i = 0; i < 2; i++) {
        EGLint attrs[] = { EGL_WIDTH, (EGLint)image_width, EGL_HEIGHT, (EGLint)image_rows,
            EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
            EGL_DMA_BUF_PLANE0_FD_EXT, fds[i], EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)(image_width * 4), EGL_NONE };
        images[slot][i] = create_image(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
        if (images[slot][i] == EGL_NO_IMAGE_KHR) {
            fprintf(stderr, "GPU_BOB DMA-BUF import slot=%u plane=%u egl=%#x\n", slot, i, eglGetError()); return -1;
        }
        glBindTexture(GL_TEXTURE_2D, textures[slot][i]);
        image_storage(GL_TEXTURE_2D, images[slot][i], NULL);
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) { fprintf(stderr, "GPU_BOB image storage slot=%u plane=%u gl=%#x\n", slot, i, error); return -1; }
    }
    return 0;
}

int qua_gpu_bob_dmabuf(unsigned slot)
{
    if (!dmabuf_mode || slot >= 64 || !textures[slot][0] || !textures[slot][1]) return -1;
    glBindImageTexture(0, textures[slot][0], 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(1, textures[slot][1], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glDispatchCompute((words + 63) / 64, 1, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    glFinish();
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) { fprintf(stderr, "GPU_BOB dispatch GL=%#x slot=%u\n", error, slot); return -1; }
    return 0;
}

int qua_gpu_bob_frame(void *dst, const void *src, size_t size)
{
    if (size != frame_bytes) { fprintf(stderr, "GPU_BOB size mismatch %zu != %zu\n", size, frame_bytes); return -1; }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[0]);
    if (mapped_upload) {
        void *in = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, frame_bytes,
                                    GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
        if (!in) return -1;
        memcpy(in, src, frame_bytes);
        if (!glUnmapBuffer(GL_SHADER_STORAGE_BUFFER)) return -1;
    } else {
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, frame_bytes, src);
    }
    glDispatchCompute((words + 63) / 64, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[1]);
    void *mapped = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, frame_bytes, GL_MAP_READ_BIT);
    if (!mapped) return -1;
    memcpy(dst, mapped, frame_bytes);
    if (!glUnmapBuffer(GL_SHADER_STORAGE_BUFFER) || glGetError() != GL_NO_ERROR) return -1;
    return 0;
}

void qua_gpu_bob_close(void)
{
    if (display == EGL_NO_DISPLAY) return;
    if (context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE) {
        for (unsigned slot = 0; slot < 64; slot++) {
            if (textures[slot][0] || textures[slot][1]) glDeleteTextures(2, textures[slot]);
            for (unsigned i = 0; i < 2; i++) if (images[slot][i] != EGL_NO_IMAGE_KHR)
                destroy_image(display, images[slot][i]);
        }
        glDeleteBuffers(2, buffers); if (program) glDeleteProgram(program);
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    eglTerminate(display);
}
