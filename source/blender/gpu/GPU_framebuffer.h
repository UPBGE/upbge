/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2005 Blender Foundation.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Brecht Van Lommel.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file GPU_framebuffer.h
 *  \ingroup gpu
 */

#ifndef __GPU_FRAMEBUFFER_H__
#define __GPU_FRAMEBUFFER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "GPU_texture.h"

typedef struct GPUFrameBuffer GPUFrameBuffer;
typedef struct GPURenderBuffer GPURenderBuffer;
typedef struct GPUOffScreen GPUOffScreen;

/* GPU Framebuffer
 * - this is a wrapper for an OpenGL framebuffer object (FBO). in practice
 *   multiple FBO's may be created, to get around limitations on the number
 *   of attached textures and the dimension requirements.
 * - after any of the GPU_framebuffer_* functions, GPU_framebuffer_restore must
 *   be called before rendering to the window framebuffer again */

void GPU_texture_bind_as_framebuffer(struct GPUTexture *tex);

GPUFrameBuffer *GPU_framebuffer_create(void);
bool GPU_framebuffer_texture_attach(GPUFrameBuffer *fb, struct GPUTexture *tex, int slot, int mip);
bool GPU_framebuffer_texture_layer_attach(
        GPUFrameBuffer *fb, struct GPUTexture *tex, int slot, int layer, int mip);
bool GPU_framebuffer_texture_cubeface_attach(
        GPUFrameBuffer *fb, struct GPUTexture *tex, int slot, int face, int mip);
int GPU_framebuffer_texture_attach_target(GPUFrameBuffer *fb, struct GPUTexture *tex, int target, int slot, int mip, bool forcet2d);
void GPU_framebuffer_texture_detach(struct GPUTexture *tex);
void GPU_framebuffer_texture_detach_target(struct GPUTexture *tex, int target);
void GPU_framebuffer_bind(GPUFrameBuffer *fb);
void GPU_framebuffer_slots_bind(GPUFrameBuffer *fb, int slot);
void GPU_framebuffer_texture_unbind(GPUFrameBuffer *fb, struct GPUTexture *tex);
void GPU_framebuffer_free(GPUFrameBuffer *fb);
bool GPU_framebuffer_check_valid(GPUFrameBuffer *fb, char err_out[256]);

int GPU_framebuffer_renderbuffer_attach(GPUFrameBuffer *fb, GPURenderBuffer *rb, int slot, char err_out[256]);
void GPU_framebuffer_renderbuffer_detach(GPURenderBuffer *rb);

void GPU_framebuffer_bind_no_save(GPUFrameBuffer *fb, int slot);
void GPU_framebuffer_bind_simple(GPUFrameBuffer *fb);
void GPU_framebuffer_bind_all_attachments(GPUFrameBuffer *fb);

bool GPU_framebuffer_bound(GPUFrameBuffer *fb);

void GPU_framebuffer_restore(void);
void GPU_framebuffer_blur(
        GPUFrameBuffer *fb, struct GPUTexture *tex,
        GPUFrameBuffer *blurfb, struct GPUTexture *blurtex, float sharpness);

/********************Game engine*******************/
typedef enum GPUFrameBufferType {
	GPU_FRAMEBUFFER_FILTER0 = 0,
	GPU_FRAMEBUFFER_FILTER1,
	GPU_FRAMEBUFFER_EYE_LEFT0,
	GPU_FRAMEBUFFER_EYE_RIGHT0,
	GPU_FRAMEBUFFER_EYE_LEFT1,
	GPU_FRAMEBUFFER_EYE_RIGHT1,
	GPU_FRAMEBUFFER_IMRENDER0,
	GPU_FRAMEBUFFER_IMRENDER1,
	GPU_FRAMEBUFFER_BLOOM0,
	GPU_FRAMEBUFFER_BLOOM1,
	GPU_FRAMEBUFFER_BLUR0,
	GPU_FRAMEBUFFER_BLUR1,
	GPU_FRAMEBUFFER_DOF0,
	GPU_FRAMEBUFFER_DOF1,
	GPU_FRAMEBUFFER_BLIT_DEPTH,
	GPU_FRAMEBUFFER_MAX,

	GPU_FRAMEBUFFER_CUSTOM,
} GPUFrameBufferType;

int GPU_framebuffer_color_bindcode(const GPUFrameBuffer *fb);
GPUTexture *GPU_framebuffer_color_texture(const GPUFrameBuffer *fb);
GPUTexture *GPU_framebuffer_depth_texture(const GPUFrameBuffer *fb);
void GPU_framebuffer_mipmap_texture(GPUFrameBuffer *fb);
void GPU_framebuffer_unmipmap_texture(GPUFrameBuffer *fb);
GPUFrameBufferType GPU_framebuffer_get_bge_type(GPUFrameBuffer *fb);
void GPU_framebuffer_set_bge_type(GPUFrameBuffer *fb, GPUFrameBufferType type);
/****************End of Game engine****************/

typedef enum GPURenderBufferType {
	GPU_RENDERBUFFER_COLOR = 0,
	GPU_RENDERBUFFER_DEPTH = 1,
} GPURenderBufferType;

GPURenderBuffer *GPU_renderbuffer_create(int width, int height, int samples, GPUTextureFormat data_type, GPURenderBufferType type, char err_out[256]);
void GPU_renderbuffer_free(GPURenderBuffer *rb);
GPUFrameBuffer *GPU_renderbuffer_framebuffer(GPURenderBuffer *rb);
int GPU_renderbuffer_framebuffer_attachment(GPURenderBuffer *rb);
void GPU_renderbuffer_framebuffer_set(GPURenderBuffer *rb, GPUFrameBuffer *fb, int attachement);
int GPU_renderbuffer_bindcode(const GPURenderBuffer *rb);
bool GPU_renderbuffer_depth(const GPURenderBuffer *rb);
int GPU_renderbuffer_width(const GPURenderBuffer *rb);
int GPU_renderbuffer_height(const GPURenderBuffer *rb);


void GPU_framebuffer_blit(
        GPUFrameBuffer *fb_read, int read_slot,
        GPUFrameBuffer *fb_write, int write_slot, bool use_depth);

void GPU_framebuffer_recursive_downsample(
        GPUFrameBuffer *fb, struct GPUTexture *tex, int num_iter,
        void (*callback)(void *userData, int level), void *userData);

/* GPU OffScreen
 * - wrapper around framebuffer and texture for simple offscreen drawing
 * - changes size if graphics card can't support it */

typedef enum GPUOffScreenMode {
	GPU_OFFSCREEN_MODE_NONE = 0,
	GPU_OFFSCREEN_RENDERBUFFER_COLOR = 1 << 0,
	GPU_OFFSCREEN_RENDERBUFFER_DEPTH = 1 << 1,
	GPU_OFFSCREEN_DEPTH_COMPARE = 1 << 2,
} GPUOffScreenMode;

GPUOffScreen *GPU_offscreen_create(int width, int height, int samples, GPUTextureFormat data_type, int mode, char err_out[256]);
void GPU_offscreen_free(GPUOffScreen *ofs);
void GPU_offscreen_bind(GPUOffScreen *ofs, bool save);
void GPU_offscreen_bind_simple(GPUOffScreen *ofs);
void GPU_offscreen_unbind(GPUOffScreen *ofs, bool restore);
void GPU_offscreen_read_pixels(GPUOffScreen *ofs, int type, void *pixels);
void GPU_offscreen_blit(GPUOffScreen *srcofs, GPUOffScreen *dstofs, bool color, bool depth);
int GPU_offscreen_width(const GPUOffScreen *ofs);
int GPU_offscreen_height(const GPUOffScreen *ofs);
int GPU_offscreen_color_texture(const GPUOffScreen *ofs);

void GPU_offscreen_viewport_data_get(
        GPUOffScreen *ofs,
        GPUFrameBuffer **r_fb, struct GPUTexture **r_color, struct GPUTexture **r_depth);

#ifdef __cplusplus
}
#endif

#endif  /* __GPU_FRAMEBUFFER_H__ */
