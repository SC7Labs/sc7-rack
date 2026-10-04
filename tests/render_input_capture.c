/* Opt-in, test-only isolated input sampling for pinned wlroots 0.17.4. */
#define _GNU_SOURCE
#include "render_input_capture.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/render/gles2.h>

struct saved_state {
    bool es3, pack_subimage, external;
    PFNGLGENVERTEXARRAYSOESPROC gen_vertex_arrays;
    PFNGLBINDVERTEXARRAYOESPROC bind_vertex_array;
    PFNGLDELETEVERTEXARRAYSOESPROC delete_vertex_arrays;
    GLint program, active_texture, texture_2d, texture_external;
    GLint draw_fbo, read_fbo, viewport[4], array_buffer;
    GLint pack_alignment, pack_row_length, pack_skip_rows, pack_skip_pixels;
    GLint pack_buffer, unpack_buffer, sampler, vertex_array;
    GLint attrib_enabled, attrib_size, attrib_type, attrib_normalized;
    GLint attrib_stride, attrib_buffer;
    void *attrib_pointer;
    GLboolean color_mask[4];
    GLboolean blend, scissor, depth, stencil, cull, dither;
    GLboolean sample_alpha, sample_coverage, rasterizer_discard;
};

static void set_reason(char *reason, size_t size, const char *message) {
    if (reason && size) {
        snprintf(reason, size, "%s", message);
    }
}

static bool has_extension(const char *list, const char *extension) {
    if (!list) {
        return false;
    }
    size_t size = strlen(extension);
    for (const char *p = strstr(list, extension); p; p = strstr(p + size, extension)) {
        if ((p == list || p[-1] == ' ') && (p[size] == '\0' || p[size] == ' ')) {
            return true;
        }
    }
    return false;
}

static void save_state(struct saved_state *state) {
    const char *version = (const char *)glGetString(GL_VERSION);
    state->es3 = version && (strstr(version, "OpenGL ES 3.") != NULL);
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    state->external = has_extension(extensions, "GL_OES_EGL_image_external");
    state->pack_subimage = state->es3 ||
        has_extension(extensions, "GL_NV_pack_subimage");
    if (state->es3) {
        state->gen_vertex_arrays = glGenVertexArrays;
        state->bind_vertex_array = glBindVertexArray;
        state->delete_vertex_arrays = glDeleteVertexArrays;
    } else if (has_extension(extensions, "GL_OES_vertex_array_object")) {
        state->gen_vertex_arrays = (void *)eglGetProcAddress("glGenVertexArraysOES");
        state->bind_vertex_array = (void *)eglGetProcAddress("glBindVertexArrayOES");
        state->delete_vertex_arrays = (void *)eglGetProcAddress("glDeleteVertexArraysOES");
        if (!state->gen_vertex_arrays || !state->bind_vertex_array ||
                !state->delete_vertex_arrays) {
            state->gen_vertex_arrays = NULL;
            state->bind_vertex_array = NULL;
            state->delete_vertex_arrays = NULL;
        }
    }
    glGetIntegerv(GL_CURRENT_PROGRAM, &state->program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &state->active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &state->texture_2d);
    if (state->external) {
        glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &state->texture_external);
    }
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &state->draw_fbo);
    if (state->es3) {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &state->read_fbo);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &state->pack_buffer);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &state->unpack_buffer);
        glGetIntegerv(GL_SAMPLER_BINDING, &state->sampler);
        state->rasterizer_discard = glIsEnabled(GL_RASTERIZER_DISCARD);
    }
    if (state->bind_vertex_array) {
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING_OES, &state->vertex_array);
    }
    glGetIntegerv(GL_VIEWPORT, state->viewport);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &state->array_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &state->pack_alignment);
    if (state->pack_subimage) {
        glGetIntegerv(GL_PACK_ROW_LENGTH, &state->pack_row_length);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &state->pack_skip_rows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &state->pack_skip_pixels);
    }
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &state->attrib_enabled);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &state->attrib_size);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &state->attrib_type);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &state->attrib_normalized);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &state->attrib_stride);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &state->attrib_buffer);
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &state->attrib_pointer);
    glGetBooleanv(GL_COLOR_WRITEMASK, state->color_mask);
    state->blend = glIsEnabled(GL_BLEND);
    state->scissor = glIsEnabled(GL_SCISSOR_TEST);
    state->depth = glIsEnabled(GL_DEPTH_TEST);
    state->stencil = glIsEnabled(GL_STENCIL_TEST);
    state->cull = glIsEnabled(GL_CULL_FACE);
    state->dither = glIsEnabled(GL_DITHER);
    state->sample_alpha = glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE);
    state->sample_coverage = glIsEnabled(GL_SAMPLE_COVERAGE);
}

static void restore_enable(GLenum capability, GLboolean enabled) {
    if (enabled) {
        glEnable(capability);
    } else {
        glDisable(capability);
    }
}

static void restore_state(const struct saved_state *state) {
    glUseProgram(state->program);
    glBindTexture(GL_TEXTURE_2D, state->texture_2d);
    if (state->external) {
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, state->texture_external);
    }
    if (state->es3) {
        glBindSampler(0, state->sampler);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state->draw_fbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, state->read_fbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, state->pack_buffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, state->unpack_buffer);
        restore_enable(GL_RASTERIZER_DISCARD, state->rasterizer_discard);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, state->draw_fbo);
    }
    glViewport(state->viewport[0], state->viewport[1],
        state->viewport[2], state->viewport[3]);
    glPixelStorei(GL_PACK_ALIGNMENT, state->pack_alignment);
    if (state->pack_subimage) {
        glPixelStorei(GL_PACK_ROW_LENGTH, state->pack_row_length);
        glPixelStorei(GL_PACK_SKIP_ROWS, state->pack_skip_rows);
        glPixelStorei(GL_PACK_SKIP_PIXELS, state->pack_skip_pixels);
    }
    if (state->bind_vertex_array) {
        /* All capture attributes are in a private VAO where supported. */
        state->bind_vertex_array(state->vertex_array);
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, state->attrib_buffer);
        glVertexAttribPointer(0, state->attrib_size, state->attrib_type,
            state->attrib_normalized, state->attrib_stride, state->attrib_pointer);
        if (state->attrib_enabled) {
            glEnableVertexAttribArray(0);
        } else {
            glDisableVertexAttribArray(0);
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, state->array_buffer);
    glColorMask(state->color_mask[0], state->color_mask[1],
        state->color_mask[2], state->color_mask[3]);
    restore_enable(GL_BLEND, state->blend);
    restore_enable(GL_SCISSOR_TEST, state->scissor);
    restore_enable(GL_DEPTH_TEST, state->depth);
    restore_enable(GL_STENCIL_TEST, state->stencil);
    restore_enable(GL_CULL_FACE, state->cull);
    restore_enable(GL_DITHER, state->dither);
    restore_enable(GL_SAMPLE_ALPHA_TO_COVERAGE, state->sample_alpha);
    restore_enable(GL_SAMPLE_COVERAGE, state->sample_coverage);
    glActiveTexture(state->active_texture);
}

static GLuint make_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool write_images(const char *path, const unsigned char *pixels,
        uint32_t width, uint32_t height) {
    char temporary[PATH_MAX], alpha_path[PATH_MAX], alpha_temporary[PATH_MAX];
    int size_rgb = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    int size_alpha = snprintf(alpha_path, sizeof(alpha_path), "%s.alpha.pgm", path);
    int size_alpha_tmp = snprintf(alpha_temporary, sizeof(alpha_temporary),
        "%s.alpha.pgm.tmp", path);
    if (size_rgb <= 0 || (size_t)size_rgb >= sizeof(temporary) ||
            size_alpha <= 0 || (size_t)size_alpha >= sizeof(alpha_path) ||
            size_alpha_tmp <= 0 || (size_t)size_alpha_tmp >= sizeof(alpha_temporary)) {
        return false;
    }
    FILE *file = fopen(temporary, "wb");
    if (!file) {
        return false;
    }
    FILE *alpha_file = fopen(alpha_temporary, "wb");
    if (!alpha_file) {
        fclose(file);
        unlink(temporary);
        return false;
    }
    bool ok = fprintf(file, "P6\n%u %u\n255\n", width, height) > 0 &&
        fprintf(alpha_file, "P5\n%u %u\n255\n", width, height) > 0;
    unsigned char *row = malloc((size_t)width * 4);
    ok = ok && row;
    for (uint32_t y = 0; ok && y < height; ++y) {
        const unsigned char *source = pixels + (size_t)(height - y - 1) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            memcpy(row + (size_t)x * 3, source + (size_t)x * 4, 3);
            row[(size_t)width * 3 + x] = source[(size_t)x * 4 + 3];
        }
        ok = fwrite(row, 3, width, file) == width &&
            fwrite(row + (size_t)width * 3, 1, width, alpha_file) == width;
    }
    free(row);
    if (fclose(file)) {
        ok = false;
    }
    if (fclose(alpha_file)) {
        ok = false;
    }
    /* There is no filesystem operation that atomically renames two files.
     * Publish PPM last: its presence marks a fully written pair to the caller. */
    bool alpha_published = false;
    if (ok) {
        alpha_published = rename(alpha_temporary, alpha_path) == 0;
        ok = alpha_published && rename(temporary, path) == 0;
    }
    if (!ok) {
        unlink(temporary);
        unlink(alpha_temporary);
        if (alpha_published) {
            unlink(alpha_path);
            unlink(path);
        }
    }
    return ok;
}

enum sc7_render_input_capture_result sc7_render_capture_input_gles2(
        struct wlr_renderer *renderer, struct wlr_texture *texture, const char *path,
        char *reason, size_t reason_size) {
    if (!renderer || !texture || !path || !*path ||
            !wlr_renderer_is_gles2(renderer) || !wlr_texture_is_gles2(texture)) {
        set_reason(reason, reason_size, "not-gles2");
        return SC7_RENDER_INPUT_CAPTURE_UNSUPPORTED;
    }
    if (!glGetString(GL_VERSION)) {
        set_reason(reason, reason_size, "no-current-context");
        return SC7_RENDER_INPUT_CAPTURE_UNSUPPORTED;
    }
    uint32_t width = texture->width, height = texture->height;
    if (width == 0 || height == 0 || width > 4096 || height > 4096) {
        set_reason(reason, reason_size, "dimensions");
        return SC7_RENDER_INPUT_CAPTURE_UNSUPPORTED;
    }
    struct wlr_gles2_texture_attribs texture_attribs;
    wlr_gles2_texture_get_attribs(texture, &texture_attribs);
    if ((texture_attribs.target != GL_TEXTURE_2D &&
            texture_attribs.target != GL_TEXTURE_EXTERNAL_OES) || !texture_attribs.tex) {
        set_reason(reason, reason_size, "texture-target");
        return SC7_RENDER_INPUT_CAPTURE_UNSUPPORTED;
    }
    if (glGetError() != GL_NO_ERROR) {
        set_reason(reason, reason_size, "prior-gl-error");
        return SC7_RENDER_INPUT_CAPTURE_ERROR;
    }
    struct saved_state saved = {0};
    save_state(&saved);
    bool external = texture_attribs.target == GL_TEXTURE_EXTERNAL_OES;
    if (external && !saved.external) {
        restore_state(&saved);
        set_reason(reason, reason_size, "external-extension-unavailable");
        return SC7_RENDER_INPUT_CAPTURE_UNSUPPORTED;
    }
    GLuint program = 0, vertex_shader = 0, fragment_shader = 0;
    GLuint fbo = 0, output_texture = 0, vertices = 0, vertex_array = 0;
    GLint min_filter = 0, mag_filter = 0;
    bool filters_changed = false;
    unsigned char *pixels = malloc((size_t)width * height * 4);
    bool ok = false;
    const char *failure = "allocation";
    if (!pixels) {
        goto cleanup;
    }
    const char *vertex_source =
        "attribute vec2 position; varying vec2 uv; void main() {"
        "gl_Position=vec4(position,0.0,1.0);"
        "uv=vec2((position.x+1.0)*0.5,(1.0-position.y)*0.5);}";
    const char *prefix = external ?
        "#extension GL_OES_EGL_image_external : require\n" : "";
    char fragment_source[512];
    snprintf(fragment_source, sizeof(fragment_source),
        "%s#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n"
        "#else\nprecision mediump float;\n#endif\n"
        "varying vec2 uv; uniform %s source; void main(){"
        "gl_FragColor=texture2D(source,uv);%s}", prefix,
        external ? "samplerExternalOES" : "sampler2D",
        texture_attribs.has_alpha ? "" : "gl_FragColor.a=1.0;");
    vertex_shader = make_shader(GL_VERTEX_SHADER, vertex_source);
    fragment_shader = make_shader(GL_FRAGMENT_SHADER, fragment_source);
    if (!vertex_shader || !fragment_shader) {
        failure = "shader-compile";
        goto cleanup;
    }
    program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        failure = "shader-link";
        goto cleanup;
    }
    if (saved.es3) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glBindSampler(0, 0);
        glDisable(GL_RASTERIZER_DISCARD);
    }
    if (saved.gen_vertex_arrays) {
        saved.gen_vertex_arrays(1, &vertex_array);
        saved.bind_vertex_array(vertex_array);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (saved.pack_subimage) {
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    }
    glGenTextures(1, &output_texture);
    glBindTexture(GL_TEXTURE_2D, output_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, output_texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        failure = "framebuffer-incomplete";
        goto cleanup;
    }
    static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenBuffers(1, &vertices);
    glBindBuffer(GL_ARRAY_BUFFER, vertices);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glEnableVertexAttribArray(0);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "source"), 0);
    glBindTexture(texture_attribs.target, texture_attribs.tex);
    glGetTexParameteriv(texture_attribs.target, GL_TEXTURE_MIN_FILTER, &min_filter);
    glGetTexParameteriv(texture_attribs.target, GL_TEXTURE_MAG_FILTER, &mag_filter);
    /* This can be the texture's first scene sample: its default minifier may
     * require mipmaps that imported client storage does not provide. */
    glTexParameteri(texture_attribs.target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(texture_attribs.target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    filters_changed = true;
    glViewport(0, 0, width, height);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glDisable(GL_SAMPLE_COVERAGE);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    ok = glGetError() == GL_NO_ERROR;
    failure = "gl-sample-or-readback";
cleanup:
    if (filters_changed) {
        glTexParameteri(texture_attribs.target, GL_TEXTURE_MIN_FILTER, min_filter);
        glTexParameteri(texture_attribs.target, GL_TEXTURE_MAG_FILTER, mag_filter);
    }
    restore_state(&saved);
    if (vertex_array) saved.delete_vertex_arrays(1, &vertex_array);
    glDeleteBuffers(1, &vertices);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &output_texture);
    if (program) glDeleteProgram(program);
    if (vertex_shader) glDeleteShader(vertex_shader);
    if (fragment_shader) glDeleteShader(fragment_shader);
    if (glGetError() != GL_NO_ERROR) {
        ok = false;
        failure = "gl-state-restore";
    }
    if (ok && !write_images(path, pixels, width, height)) {
        ok = false;
        failure = "image-write";
    }
    free(pixels);
    set_reason(reason, reason_size, ok ? "captured" : failure);
    return ok ? SC7_RENDER_INPUT_CAPTURE_OK : SC7_RENDER_INPUT_CAPTURE_ERROR;
}
