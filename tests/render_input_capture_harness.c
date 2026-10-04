/* Real surfaceless Mesa GLES sampling, with stand-in wlroots type predicates. */
#define _GNU_SOURCE
#include "render_input_capture.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/render/gles2.h>

static struct wlr_renderer standin_renderer;
static struct wlr_texture standin_texture = {.width = 3, .height = 2};
static struct wlr_gles2_texture_attribs texture_attribs;
static bool fail_fbo;
static int fail_rename_at, rename_count;
int __real_rename(const char *old, const char *new);
int __wrap_rename(const char *old, const char *new) {
    if (fail_rename_at && ++rename_count == fail_rename_at) {
        errno = EIO;
        return -1;
    }
    return __real_rename(old, new);
}
GLenum __real_glCheckFramebufferStatus(GLenum target);
GLenum __wrap_glCheckFramebufferStatus(GLenum target) {
    return fail_fbo ? GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT :
        __real_glCheckFramebufferStatus(target);
}

bool wlr_renderer_is_gles2(struct wlr_renderer *renderer) {
    return renderer == &standin_renderer;
}
bool wlr_texture_is_gles2(struct wlr_texture *texture) {
    return texture == &standin_texture;
}
void wlr_gles2_texture_get_attribs(struct wlr_texture *texture,
        struct wlr_gles2_texture_attribs *attribs) {
    assert(texture == &standin_texture);
    *attribs = texture_attribs;
}

struct snapshot {
    GLint program, active, texture_2d, external, array_buffer, renderbuffer;
    GLint draw_fbo, read_fbo, viewport[4], scissor[4];
    GLint pack_alignment, unpack_alignment, pack_row, pack_rows, pack_pixels;
    GLint pack_buffer, unpack_buffer, sampler, vao;
    GLint attrib[9];
    void *pointer;
    GLfloat constant[4], blend_color[4];
    GLint blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha;
    GLint blend_eq_rgb, blend_eq_alpha;
    GLboolean mask[4], enabled[9];
    GLint min_filter, mag_filter;
};

static bool es3, oes_vao;
static const GLenum capabilities[] = {GL_BLEND, GL_SCISSOR_TEST, GL_DEPTH_TEST,
    GL_STENCIL_TEST, GL_CULL_FACE, GL_DITHER, GL_SAMPLE_ALPHA_TO_COVERAGE,
    GL_SAMPLE_COVERAGE, GL_RASTERIZER_DISCARD};

static void take_snapshot(struct snapshot *s) {
    memset(s, 0, sizeof(*s));
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->array_buffer);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &s->renderbuffer);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s->draw_fbo);
    glGetIntegerv(GL_VIEWPORT, s->viewport);
    glGetIntegerv(GL_SCISSOR_BOX, s->scissor);
    glGetIntegerv(GL_PACK_ALIGNMENT, &s->pack_alignment);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &s->unpack_alignment);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s->blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &s->blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s->blend_src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s->blend_dst_alpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &s->blend_eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s->blend_eq_alpha);
    glGetFloatv(GL_BLEND_COLOR, s->blend_color);
    glGetBooleanv(GL_COLOR_WRITEMASK, s->mask);
    size_t count = es3 ? 9 : 8;
    for (size_t i = 0; i < count; ++i) s->enabled[i] = glIsEnabled(capabilities[i]);
    static const GLenum fields[] = {GL_VERTEX_ATTRIB_ARRAY_ENABLED,
        GL_VERTEX_ATTRIB_ARRAY_SIZE, GL_VERTEX_ATTRIB_ARRAY_TYPE,
        GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, GL_VERTEX_ATTRIB_ARRAY_STRIDE,
        GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, GL_VERTEX_ATTRIB_ARRAY_INTEGER,
        GL_VERTEX_ATTRIB_ARRAY_DIVISOR};
    count = es3 ? 8 : 6;
    for (size_t i = 0; i < count; ++i) glGetVertexAttribiv(0, fields[i], &s->attrib[i]);
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &s->pointer);
    glGetVertexAttribfv(0, GL_CURRENT_VERTEX_ATTRIB, s->constant);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->texture_2d);
    glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &s->external);
    if (es3) {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->read_fbo);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &s->pack_buffer);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &s->unpack_buffer);
        glGetIntegerv(GL_SAMPLER_BINDING, &s->sampler);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &s->pack_row);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &s->pack_rows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &s->pack_pixels);
    }
    if (oes_vao) glGetIntegerv(GL_VERTEX_ARRAY_BINDING_OES, &s->vao);
    glBindTexture(texture_attribs.target, texture_attribs.tex);
    glGetTexParameteriv(texture_attribs.target, GL_TEXTURE_MIN_FILTER, &s->min_filter);
    glGetTexParameteriv(texture_attribs.target, GL_TEXTURE_MAG_FILTER, &s->mag_filter);
    glBindTexture(texture_attribs.target,
        texture_attribs.target == GL_TEXTURE_2D ? s->texture_2d : s->external);
    glActiveTexture(s->active);
    assert(glGetError() == GL_NO_ERROR);
}

static GLuint make_program(void) {
    const char *vsrc = "attribute vec2 p; void main(){gl_Position=vec4(p,0,1);}";
    const char *fsrc = "precision mediump float; void main(){gl_FragColor=vec4(1);}";
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &vsrc, NULL); glCompileShader(v);
    glShaderSource(f, 1, &fsrc, NULL); glCompileShader(f);
    GLuint program = glCreateProgram();
    glAttachShader(program, v); glAttachShader(program, f); glLinkProgram(program);
    glDeleteShader(v); glDeleteShader(f);
    return program;
}

static void polluted_state(void) {
    GLuint objects[3];
    glUseProgram(make_program());
    glGenTextures(3, objects);
    glBindTexture(GL_TEXTURE_2D, objects[0]);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, objects[1]);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, objects[2]);
    glGenFramebuffers(2, objects);
    if (es3) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, objects[0]);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, objects[1]);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, objects[0]);
    }
    glGenRenderbuffers(1, objects);
    glBindRenderbuffer(GL_RENDERBUFFER, objects[0]);
    glGenBuffers(2, objects);
    if (es3) {
        GLuint vao;
        glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    } else if (oes_vao) {
        PFNGLGENVERTEXARRAYSOESPROC gen = (void *)eglGetProcAddress("glGenVertexArraysOES");
        PFNGLBINDVERTEXARRAYOESPROC bind = (void *)eglGetProcAddress("glBindVertexArrayOES");
        GLuint vao;
        gen(1, &vao); bind(vao);
    }
    glBindBuffer(GL_ARRAY_BUFFER, objects[0]);
    glBufferData(GL_ARRAY_BUFFER, 128, NULL, GL_STATIC_DRAW);
    if (es3) {
        glVertexAttribIPointer(0, 3, GL_SHORT, 8, (void *)16);
        glVertexAttribDivisor(0, 5);
    } else {
        glVertexAttribPointer(0, 3, GL_SHORT, GL_TRUE, 8, (void *)16);
    }
    glEnableVertexAttribArray(0);
    glVertexAttrib4f(0, 0.2f, 0.3f, 0.4f, 0.5f);
    glBindBuffer(GL_ARRAY_BUFFER, objects[1]);
    glViewport(7, 11, 83, 91);
    glScissor(2, 5, 17, 13);
    glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_CONSTANT_COLOR, GL_DST_ALPHA, GL_ONE);
    glBlendEquationSeparate(GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT);
    glBlendColor(0.1f, 0.2f, 0.3f, 0.4f);
    for (size_t i = 0; i < (es3 ? 9u : 8u); ++i) glEnable(capabilities[i]);
    glPixelStorei(GL_PACK_ALIGNMENT, 8);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    if (es3) {
        glGenBuffers(2, objects);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, objects[0]);
        glBufferData(GL_PIXEL_PACK_BUFFER, 8192, NULL, GL_STATIC_DRAW);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, objects[1]);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, 8192, NULL, GL_STATIC_DRAW);
        glPixelStorei(GL_PACK_ROW_LENGTH, 53);
        glPixelStorei(GL_PACK_SKIP_ROWS, 3);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 2);
        GLuint sampler;
        glGenSamplers(1, &sampler); glBindSampler(0, sampler);
    }
    assert(glGetError() == GL_NO_ERROR);
}

static const unsigned char input_pixels[] = {
    255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255,
    255, 255, 0, 255, 255, 0, 255, 255, 0, 255, 255, 255,
};
/* Alpha=0 RGB can be irrelevant stale data. Preserve it and alpha separately
 * rather than diagnosing corruption from that RGB alone. Other pixels premul. */
static const unsigned char alpha_pixels[] = {
    173, 99, 241, 0, 64, 32, 16, 128,
    8, 4, 2, 16, 42, 11, 87, 255,
};
static const unsigned char *expected_pixels = input_pixels;

static void check_image(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file);
    char magic[3]; int width, height, max;
    assert(fscanf(file, "%2s %d %d %d", magic, &width, &height, &max) == 4);
    assert(strcmp(magic, "P6") == 0 && width == (int)standin_texture.width &&
        height == (int)standin_texture.height && max == 255);
    assert(fgetc(file) == '\n');
    for (size_t pixel = 0; pixel < (size_t)width * height; ++pixel) {
        unsigned char rgb[3];
        assert(fread(rgb, 3, 1, file) == 1);
        assert(memcmp(rgb, expected_pixels + pixel * 4, 3) == 0);
    }
    assert(fgetc(file) == EOF);
    fclose(file);
    char alpha_path[4096];
    assert(snprintf(alpha_path, sizeof(alpha_path), "%s.alpha.pgm", path) > 0);
    file = fopen(alpha_path, "rb");
    assert(file);
    int alpha_width, alpha_height;
    assert(fscanf(file, "%2s %d %d %d", magic, &alpha_width, &alpha_height, &max) == 4);
    assert(strcmp(magic, "P5") == 0 && alpha_width == width && alpha_height == height && max == 255);
    assert(fgetc(file) == '\n');
    for (size_t pixel = 0; pixel < (size_t)width * height; ++pixel) {
        unsigned char alpha = texture_attribs.has_alpha ? expected_pixels[pixel * 4 + 3] : 255;
        assert(fgetc(file) == alpha);
    }
    assert(fgetc(file) == EOF);
    fclose(file);
}

int main(int argc, char **argv) {
    assert(argc == 3);
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay display = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA,
        EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) return 77;
    assert(eglBindAPI(EGL_OPENGL_ES_API));
    EGLint configs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES2_BIT, EGL_NONE};
    EGLConfig config; EGLint count;
    assert(eglChooseConfig(display, configs, &config, 1, &count) && count == 1);
    EGLint attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, attributes);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE,
            EGL_NO_SURFACE, context)) return 77;
    es3 = strstr((const char *)glGetString(GL_VERSION), "OpenGL ES 3.") != NULL;
    oes_vao = strstr((const char *)glGetString(GL_EXTENSIONS),
        "GL_OES_vertex_array_object") != NULL;
    glGenTextures(1, &texture_attribs.tex);
    texture_attribs.target = GL_TEXTURE_2D; texture_attribs.has_alpha = true;
    if (strcmp(argv[1], "alpha") == 0 || strcmp(argv[1], "rgbx-alpha") == 0) {
        expected_pixels = alpha_pixels;
        standin_texture.width = 2; standin_texture.height = 2;
        texture_attribs.has_alpha = strcmp(argv[1], "rgbx-alpha") != 0;
    }
    glBindTexture(GL_TEXTURE_2D, texture_attribs.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, standin_texture.width, standin_texture.height,
        0, GL_RGBA, GL_UNSIGNED_BYTE, expected_pixels);
    if (strcmp(argv[1], "external") == 0) {
        PFNEGLCREATEIMAGEKHRPROC create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
        PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind_image =
            (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
        if (!create_image || !bind_image) return 77;
        EGLint image_attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        /* glTexImage2D's default minifier is intentionally incomplete. EGLImage
         * creation needs complete source storage; capture must restore defaults
         * on the separately imported external texture instead. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        EGLImageKHR image = create_image(display, context, EGL_GL_TEXTURE_2D_KHR,
            (EGLClientBuffer)(uintptr_t)texture_attribs.tex, image_attributes);
        if (image == EGL_NO_IMAGE_KHR) return 77;
        glGenTextures(1, &texture_attribs.tex);
        texture_attribs.target = GL_TEXTURE_EXTERNAL_OES;
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture_attribs.tex);
        bind_image(GL_TEXTURE_EXTERNAL_OES, image);
    }
    assert(glGetError() == GL_NO_ERROR);
    if (strcmp(argv[1], "polluted") == 0 || strcmp(argv[1], "write-failure") == 0 ||
            strcmp(argv[1], "repeated") == 0 || strcmp(argv[1], "external") == 0 ||
            strcmp(argv[1], "fbo-failure") == 0) polluted_state();
    struct snapshot before, after;
    take_snapshot(&before);
    char reason[80];
    if (strcmp(argv[1], "bad-dimensions") == 0) {
        standin_texture.width = 4097;
        assert(sc7_render_capture_input_gles2(&standin_renderer, &standin_texture,
            argv[2], reason, sizeof(reason)) == SC7_RENDER_INPUT_CAPTURE_UNSUPPORTED);
        assert(strcmp(reason, "dimensions") == 0);
    } else if (strcmp(argv[1], "prior-error") == 0) {
        glEnable(0x123456);
        assert(sc7_render_capture_input_gles2(&standin_renderer, &standin_texture,
            argv[2], reason, sizeof(reason)) == SC7_RENDER_INPUT_CAPTURE_ERROR);
        assert(strcmp(reason, "prior-gl-error") == 0);
    } else if (strcmp(argv[1], "not-gles") == 0) {
        assert(sc7_render_capture_input_gles2(NULL, &standin_texture,
            argv[2], reason, sizeof(reason)) == SC7_RENDER_INPUT_CAPTURE_UNSUPPORTED);
    } else {
        fail_fbo = strcmp(argv[1], "fbo-failure") == 0;
        if (strcmp(argv[1], "publish-failure-1") == 0) fail_rename_at = 1;
        if (strcmp(argv[1], "publish-failure-2") == 0) fail_rename_at = 2;
        int cycles = strcmp(argv[1], "repeated") == 0 ? 32 : 1;
        for (int i = 0; i < cycles; ++i) {
            enum sc7_render_input_capture_result result =
                sc7_render_capture_input_gles2(&standin_renderer, &standin_texture,
                    argv[2], reason, sizeof(reason));
            if (strcmp(argv[1], "write-failure") == 0 || fail_rename_at) {
                assert(result == SC7_RENDER_INPUT_CAPTURE_ERROR);
                assert(strcmp(reason, "image-write") == 0);
                char alpha_path[4096];
                snprintf(alpha_path, sizeof(alpha_path), "%s.alpha.pgm", argv[2]);
                assert(access(argv[2], F_OK) != 0 && access(alpha_path, F_OK) != 0);
            } else if (fail_fbo) {
                assert(result == SC7_RENDER_INPUT_CAPTURE_ERROR);
                assert(strcmp(reason, "framebuffer-incomplete") == 0);
            } else {
                if (result != SC7_RENDER_INPUT_CAPTURE_OK) fprintf(stderr, "%s\n", reason);
                assert(result == SC7_RENDER_INPUT_CAPTURE_OK);
                check_image(argv[2]);
            }
        }
    }
    take_snapshot(&after);
    assert(memcmp(&before, &after, sizeof(before)) == 0);
    assert(eglGetCurrentContext() == context && eglGetCurrentDisplay() == display);
    assert(eglGetCurrentSurface(EGL_DRAW) == EGL_NO_SURFACE &&
        eglGetCurrentSurface(EGL_READ) == EGL_NO_SURFACE);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context); eglTerminate(display);
    puts("PASS");
    return 0;
}
