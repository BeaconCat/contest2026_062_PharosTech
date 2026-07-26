/****************************************************************************
 * app/softgl/softgl.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * SoftGL context management: buffers, render state, the rasteriser worker
 * pool, presentation to a NuttX framebuffer device, and mesh loading (both
 * the compact binary container produced by tools/obj_to_mesh.py and a plain
 * Wavefront OBJ parser for development).
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nuttx/video/fb.h>

#include "softgl.h"

#ifdef __ARM_NEON
#  include <arm_neon.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Initial capacity of the screen-space triangle bin.  It grows on demand. */

#define SOFTGL_TRIBIN_INITIAL     2048

/* Growth headroom for the OBJ parser's dynamic arrays. */

#define SOFTGL_OBJ_INITIAL        256

/* Longest OBJ line we accept. */

#define SOFTGL_OBJ_LINE_MAX       256

/* Hard cap so that a corrupt file cannot make us allocate wildly. */

#define SOFTGL_MESH_MAX_VERTS     65535u
#define SOFTGL_MESH_MAX_INDICES   (3u * 262144u)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* De-duplication key used while parsing OBJ faces: the v/vt/vn index
 * triple that produced one emitted vertex.  -1 means "not specified".
 */

struct softgl_objkey_s
{
  int32_t v;
  int32_t t;
  int32_t n;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void *softgl_worker_entry(void *arg);
static int   softgl_pool_start(struct softgl_context_s *ctx, int nworkers);
static void  softgl_pool_stop(struct softgl_context_s *ctx);
static void  softgl_update_derived(struct softgl_context_s *ctx);
static int   softgl_obj_reserve(void **buf, uint32_t *cap, uint32_t need,
                                size_t elemsize);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: softgl_worker_entry
 *
 * Description:
 *   Rasteriser worker.  Sleeps on start_cv until the caller bumps job_seq,
 *   scan converts its own band of scanlines, then signals completion.  The
 *   band partitioning guarantees that no two threads ever touch the same
 *   colour or depth pixel, so no locking is needed inside the inner loop.
 *
 ****************************************************************************/

static void *softgl_worker_entry(void *arg)
{
  struct softgl_worker_s  *worker = (struct softgl_worker_s *)arg;
  struct softgl_context_s *ctx    = worker->ctx;

  pthread_mutex_lock(&ctx->lock);

  for (; ; )
    {
      while (!ctx->shutdown && worker->seq == ctx->job_seq)
        {
          pthread_cond_wait(&ctx->start_cv, &ctx->lock);
        }

      if (ctx->shutdown)
        {
          break;
        }

      worker->seq = ctx->job_seq;
      pthread_mutex_unlock(&ctx->lock);

      softgl_raster_band(ctx, worker->band);

      pthread_mutex_lock(&ctx->lock);
      if (--ctx->pending == 0)
        {
          pthread_cond_broadcast(&ctx->done_cv);
        }
    }

  pthread_mutex_unlock(&ctx->lock);
  return NULL;
}

/****************************************************************************
 * Name: softgl_pool_start
 ****************************************************************************/

static int softgl_pool_start(struct softgl_context_s *ctx, int nworkers)
{
  int i;
  int ret;

  ctx->shutdown = false;
  ctx->job_seq  = 0;
  ctx->pending  = 0;

  for (i = 0; i < nworkers; i++)
    {
      ctx->workers[i].ctx     = ctx;
      ctx->workers[i].band    = i + 1;
      ctx->workers[i].seq     = 0;
      ctx->workers[i].started = false;

      ret = pthread_create(&ctx->workers[i].tid, NULL, softgl_worker_entry,
                           &ctx->workers[i]);
      if (ret != 0)
        {
          /* Fall back to however many threads we did manage to create. */

          break;
        }

      ctx->workers[i].started = true;
    }

  ctx->nworkers = i;
  ctx->nbands   = i + 1;
  return OK;
}

/****************************************************************************
 * Name: softgl_pool_stop
 ****************************************************************************/

static void softgl_pool_stop(struct softgl_context_s *ctx)
{
  int i;

  pthread_mutex_lock(&ctx->lock);
  ctx->shutdown = true;
  pthread_cond_broadcast(&ctx->start_cv);
  pthread_mutex_unlock(&ctx->lock);

  for (i = 0; i < ctx->nworkers; i++)
    {
      if (ctx->workers[i].started)
        {
          pthread_join(ctx->workers[i].tid, NULL);
          ctx->workers[i].started = false;
        }
    }

  ctx->nworkers = 0;
  ctx->nbands   = 1;
}

/****************************************************************************
 * Name: softgl_update_derived
 *
 * Description:
 *   Recompute the composite transforms that the vertex stage consumes:
 *   the full model-view-projection matrix, the inverse transpose of the
 *   model matrix (for normals under non-uniform scale) and the world-space
 *   camera position (for the specular term).
 *
 ****************************************************************************/

static void softgl_update_derived(struct softgl_context_s *ctx)
{
  struct softgl_mat4_s tmp;

  softgl_mat4_mul(&tmp, &ctx->proj, &ctx->view);
  softgl_mat4_mul(&ctx->mvp, &tmp, &ctx->model);

  if (softgl_mat4_inverse(&tmp, &ctx->model))
    {
      softgl_mat4_transpose(&ctx->normalmat, &tmp);
    }
  else
    {
      ctx->normalmat = ctx->model;
    }

  if (softgl_mat4_inverse(&tmp, &ctx->view))
    {
      ctx->eye = softgl_vec3(tmp.m[12], tmp.m[13], tmp.m[14]);
    }
  else
    {
      ctx->eye = softgl_vec3(0.0f, 0.0f, 0.0f);
    }

  ctx->dirty = false;
}

/****************************************************************************
 * Name: softgl_obj_reserve
 *
 * Description:
 *   Grow a dynamic array to hold at least "need" elements.
 *
 ****************************************************************************/

static int softgl_obj_reserve(void **buf, uint32_t *cap, uint32_t need,
                              size_t elemsize)
{
  uint32_t newcap = *cap;
  void *p;

  if (need <= *cap)
    {
      return OK;
    }

  if (newcap == 0)
    {
      newcap = SOFTGL_OBJ_INITIAL;
    }

  while (newcap < need)
    {
      newcap *= 2;
    }

  p = realloc(*buf, (size_t)newcap * elemsize);
  if (p == NULL)
    {
      return -ENOMEM;
    }

  *buf = p;
  *cap = newcap;
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: softgl_fill16
 *
 * Description:
 *   Fill "count" 16-bit words with "value".  Used for colour and depth
 *   clears; NEON stores 8 words per instruction.
 *
 ****************************************************************************/

void softgl_fill16(uint16_t *dst, uint16_t value, size_t count)
{
#ifdef __ARM_NEON
  uint16x8_t v8 = vdupq_n_u16(value);

  while (count >= 32)
    {
      vst1q_u16(dst +  0, v8);
      vst1q_u16(dst +  8, v8);
      vst1q_u16(dst + 16, v8);
      vst1q_u16(dst + 24, v8);
      dst   += 32;
      count -= 32;
    }

  while (count >= 8)
    {
      vst1q_u16(dst, v8);
      dst   += 8;
      count -= 8;
    }
#endif

  while (count-- > 0)
    {
      *dst++ = value;
    }
}

/****************************************************************************
 * Name: softgl_create_context
 *
 * Description:
 *   Allocate a rendering context.  If "framebuffer" is NULL an RGB565
 *   colour buffer of width * height pixels is allocated internally,
 *   otherwise the caller's buffer is rendered into directly (this is how
 *   the demo renders straight into /dev/fb0 memory with zero copies).
 *   The 16-bit Z-buffer is always owned by the context.
 *
 * Returned Value:
 *   The new context, or NULL on failure.
 *
 ****************************************************************************/

struct softgl_context_s *softgl_create_context(int width, int height,
                                               uint16_t *framebuffer)
{
  struct softgl_context_s *ctx;
  int nthreads;

  if (width <= 0 || height <= 0)
    {
      return NULL;
    }

  ctx = (struct softgl_context_s *)calloc(1, sizeof(*ctx));
  if (ctx == NULL)
    {
      return NULL;
    }

  ctx->width  = width;
  ctx->height = height;
  ctx->stride = width;
  ctx->fbfd   = -1;

  if (framebuffer != NULL)
    {
      ctx->color     = framebuffer;
      ctx->own_color = false;
    }
  else
    {
      ctx->color = (uint16_t *)malloc((size_t)width * height *
                                      sizeof(uint16_t));
      if (ctx->color == NULL)
        {
          free(ctx);
          return NULL;
        }

      ctx->own_color = true;
    }

  ctx->depth = (uint16_t *)malloc((size_t)width * height *
                                  sizeof(uint16_t));
  if (ctx->depth == NULL)
    {
      if (ctx->own_color)
        {
          free(ctx->color);
        }

      free(ctx);
      return NULL;
    }

  ctx->own_depth = true;

  ctx->tris = (struct softgl_rtri_s *)
              malloc(SOFTGL_TRIBIN_INITIAL * sizeof(struct softgl_rtri_s));
  if (ctx->tris == NULL)
    {
      free(ctx->depth);
      if (ctx->own_color)
        {
          free(ctx->color);
        }

      free(ctx);
      return NULL;
    }

  ctx->tricap = SOFTGL_TRIBIN_INITIAL;

  /* Default render state. */

  softgl_mat4_identity(&ctx->model);
  softgl_mat4_identity(&ctx->view);
  softgl_mat4_identity(&ctx->proj);

  ctx->cull        = SOFTGL_CULL_BACK;
  ctx->filter      = SOFTGL_FILTER_BILINEAR;
  ctx->shade       = SOFTGL_SHADE_PHONG;
  ctx->depth_test  = true;
  ctx->depth_write = true;
  ctx->dirty       = true;

  ctx->light.direction = softgl_vec3_normalize(
                           softgl_vec3(-0.4f, 0.8f, 0.45f));
  ctx->light.color     = softgl_vec3(1.0f, 0.97f, 0.92f);
  ctx->light.ambient   = softgl_vec3(0.18f, 0.19f, 0.24f);
  ctx->light.specular  = 0.35f;
  ctx->light.shininess = 24.0f;

  pthread_mutex_init(&ctx->lock, NULL);
  pthread_cond_init(&ctx->start_cv, NULL);
  pthread_cond_init(&ctx->done_cv, NULL);

  ctx->nbands   = 1;
  ctx->nworkers = 0;

  /* Spread the rasteriser over every core the board reports (RK3576 has
   * four Cortex-A72 plus four Cortex-A53).
   */

#ifdef CONFIG_SMP
  nthreads = CONFIG_SMP_NCPUS;
#else
  nthreads = 1;
#endif

  softgl_set_threads(ctx, nthreads);

  softgl_update_derived(ctx);
  return ctx;
}

/****************************************************************************
 * Name: softgl_destroy_context
 ****************************************************************************/

void softgl_destroy_context(struct softgl_context_s *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  softgl_pool_stop(ctx);

  pthread_cond_destroy(&ctx->done_cv);
  pthread_cond_destroy(&ctx->start_cv);
  pthread_mutex_destroy(&ctx->lock);

  if (ctx->fbfd >= 0)
    {
      close(ctx->fbfd);
      ctx->fbfd = -1;
    }

  free(ctx->tris);
  free(ctx->vsbuf);

  if (ctx->own_depth)
    {
      free(ctx->depth);
    }

  if (ctx->own_color)
    {
      free(ctx->color);
    }

  free(ctx);
}

/****************************************************************************
 * Name: softgl_set_threads
 *
 * Description:
 *   Resize the rasteriser worker pool.  nthreads counts the total number of
 *   bands including the calling thread, so 1 means fully single threaded.
 *
 * Returned Value:
 *   The number of bands actually in use.
 *
 ****************************************************************************/

int softgl_set_threads(struct softgl_context_s *ctx, int nthreads)
{
  if (nthreads < 1)
    {
      nthreads = 1;
    }
  else if (nthreads > SOFTGL_MAX_THREADS)
    {
      nthreads = SOFTGL_MAX_THREADS;
    }

  softgl_pool_stop(ctx);

  if (nthreads > 1)
    {
      softgl_pool_start(ctx, nthreads - 1);
    }

  return ctx->nbands;
}

/****************************************************************************
 * Name: softgl_clear
 ****************************************************************************/

void softgl_clear(struct softgl_context_s *ctx, uint16_t color, bool depth)
{
  size_t npix = (size_t)ctx->width * ctx->height;

  if (ctx->stride == ctx->width)
    {
      softgl_fill16(ctx->color, color, npix);
    }
  else
    {
      int y;

      for (y = 0; y < ctx->height; y++)
        {
          softgl_fill16(ctx->color + (size_t)y * ctx->stride, color,
                        ctx->width);
        }
    }

  if (depth)
    {
      softgl_fill16(ctx->depth, (uint16_t)SOFTGL_DEPTH_MAX, npix);
    }

  ctx->stat_tris_in    = 0;
  ctx->stat_tris_drawn = 0;
  ctx->stat_pixels     = 0;
}

/****************************************************************************
 * Name: softgl_set_matrix
 ****************************************************************************/

void softgl_set_matrix(struct softgl_context_s *ctx,
                       enum softgl_matrix_e which,
                       const struct softgl_mat4_s *m)
{
  switch (which)
    {
      case SOFTGL_MATRIX_MODEL:
        ctx->model = *m;
        break;

      case SOFTGL_MATRIX_VIEW:
        ctx->view = *m;
        break;

      case SOFTGL_MATRIX_PROJECTION:
        ctx->proj = *m;
        break;

      default:
        return;
    }

  ctx->dirty = true;
}

/****************************************************************************
 * Name: softgl_bind_texture
 ****************************************************************************/

void softgl_bind_texture(struct softgl_context_s *ctx,
                         const struct softgl_texture_s *tex)
{
  ctx->texture = tex;
}

/****************************************************************************
 * Name: softgl_set_light
 ****************************************************************************/

void softgl_set_light(struct softgl_context_s *ctx,
                      const struct softgl_light_s *light)
{
  ctx->light           = *light;
  ctx->light.direction = softgl_vec3_normalize(ctx->light.direction);
}

/****************************************************************************
 * Name: softgl_set_cull
 ****************************************************************************/

void softgl_set_cull(struct softgl_context_s *ctx, enum softgl_cull_e cull)
{
  ctx->cull = cull;
}

/****************************************************************************
 * Name: softgl_set_filter
 ****************************************************************************/

void softgl_set_filter(struct softgl_context_s *ctx,
                       enum softgl_filter_e filter)
{
  ctx->filter = filter;
}

/****************************************************************************
 * Name: softgl_set_shading
 ****************************************************************************/

void softgl_set_shading(struct softgl_context_s *ctx,
                        enum softgl_shade_e shade)
{
  ctx->shade = shade;
}

/****************************************************************************
 * Name: softgl_sync_state
 *
 * Description:
 *   Refresh the derived transforms if any matrix changed.  Called by the
 *   rasteriser at the top of every draw.
 *
 ****************************************************************************/

void softgl_sync_state(struct softgl_context_s *ctx)
{
  if (ctx->dirty)
    {
      softgl_update_derived(ctx);
    }
}

/****************************************************************************
 * Name: softgl_dispatch_bands
 *
 * Description:
 *   Kick every worker on its band, rasterise band 0 on the calling thread,
 *   then wait for the workers to finish.
 *
 ****************************************************************************/

void softgl_dispatch_bands(struct softgl_context_s *ctx)
{
  if (ctx->nworkers == 0)
    {
      softgl_raster_band(ctx, 0);
      return;
    }

  pthread_mutex_lock(&ctx->lock);
  ctx->pending = ctx->nworkers;
  ctx->job_seq++;
  pthread_cond_broadcast(&ctx->start_cv);
  pthread_mutex_unlock(&ctx->lock);

  softgl_raster_band(ctx, 0);

  pthread_mutex_lock(&ctx->lock);
  while (ctx->pending > 0)
    {
      pthread_cond_wait(&ctx->done_cv, &ctx->lock);
    }

  pthread_mutex_unlock(&ctx->lock);
}

/****************************************************************************
 * Name: softgl_bind_fbdev
 *
 * Description:
 *   Open a NuttX framebuffer device and remember its mapping so that
 *   softgl_present() can blit into it.  When the panel geometry matches the
 *   context and the context owns no colour buffer of its own, rendering
 *   still goes through the context buffer; use the framebuffer pointer
 *   returned here directly in softgl_create_context() if you want a truly
 *   zero-copy path.
 *
 * Returned Value:
 *   OK, or a negated errno.
 *
 ****************************************************************************/

int softgl_bind_fbdev(struct softgl_context_s *ctx, const char *devpath)
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  int fd;

  fd = open(devpath, O_RDWR);
  if (fd < 0)
    {
      return -errno;
    }

  memset(&vinfo, 0, sizeof(vinfo));
  if (ioctl(fd, FBIOGET_VIDEOINFO, (unsigned long)((uintptr_t)&vinfo)) < 0)
    {
      close(fd);
      return -errno;
    }

  memset(&pinfo, 0, sizeof(pinfo));
  if (ioctl(fd, FBIOGET_PLANEINFO, (unsigned long)((uintptr_t)&pinfo)) < 0)
    {
      close(fd);
      return -errno;
    }

  if (pinfo.bpp != 16 || pinfo.fbmem == NULL)
    {
      /* SoftGL only produces RGB565. */

      close(fd);
      return -ENOTSUP;
    }

  ctx->fbfd     = fd;
  ctx->fbmem    = (uint16_t *)pinfo.fbmem;
  ctx->fblen    = pinfo.fblen;
  ctx->fbstride = (int)(pinfo.stride / sizeof(uint16_t));

  if (ctx->fbstride <= 0)
    {
      ctx->fbstride = vinfo.xres;
    }

  return OK;
}

/****************************************************************************
 * Name: softgl_present
 *
 * Description:
 *   Push the colour buffer to the bound framebuffer device.  When the
 *   context was created directly on top of the framebuffer memory this
 *   degenerates into the panel update ioctl only.
 *
 * Returned Value:
 *   OK, -ENODEV when no framebuffer is bound, or a negated errno.
 *
 ****************************************************************************/

int softgl_present(struct softgl_context_s *ctx)
{
#ifdef CONFIG_FB_UPDATE
  struct fb_area_s area;
#endif

  if (ctx->fbfd < 0)
    {
      return -ENODEV;
    }

  if (ctx->fbmem != ctx->color)
    {
      int y;

      for (y = 0; y < ctx->height; y++)
        {
          memcpy(ctx->fbmem + (size_t)y * ctx->fbstride,
                 ctx->color + (size_t)y * ctx->stride,
                 (size_t)ctx->width * sizeof(uint16_t));
        }
    }

#ifdef CONFIG_FB_UPDATE
  area.x = 0;
  area.y = 0;
  area.w = ctx->width;
  area.h = ctx->height;

  if (ioctl(ctx->fbfd, FBIO_UPDATE, (unsigned long)((uintptr_t)&area)) < 0)
    {
      return -errno;
    }
#endif

  return OK;
}

/****************************************************************************
 * Name: softgl_write_ppm
 *
 * Description:
 *   Dump the colour buffer as a binary PPM (P6).  Used when no panel is
 *   available so that a render can still be inspected off-board.
 *
 * Returned Value:
 *   OK, or a negated errno.
 *
 ****************************************************************************/

int softgl_write_ppm(struct softgl_context_s *ctx, const char *path)
{
  FILE *fp;
  int x;
  int y;

  fp = fopen(path, "wb");
  if (fp == NULL)
    {
      return -errno;
    }

  fprintf(fp, "P6\n%d %d\n255\n", ctx->width, ctx->height);

  for (y = 0; y < ctx->height; y++)
    {
      const uint16_t *row = ctx->color + (size_t)y * ctx->stride;

      for (x = 0; x < ctx->width; x++)
        {
          uint16_t c = row[x];
          uint8_t rgb[3];

          /* Replicate the high bits into the low ones so that full-scale
           * RGB565 maps to full-scale RGB888.
           */

          rgb[0] = (uint8_t)(((c >> 11) & 0x1f) * 255 / 31);
          rgb[1] = (uint8_t)(((c >> 5)  & 0x3f) * 255 / 63);
          rgb[2] = (uint8_t)((c & 0x1f) * 255 / 31);

          if (fwrite(rgb, 1, sizeof(rgb), fp) != sizeof(rgb))
            {
              fclose(fp);
              return -EIO;
            }
        }
    }

  fclose(fp);
  return OK;
}

/****************************************************************************
 * Name: softgl_mesh_load_memory
 *
 * Description:
 *   Parse an in-memory .mesh container.  The vertex and index arrays are
 *   copied out so that the caller may free or unmap the source buffer; use
 *   softgl_mesh_free() to release them.
 *
 * Returned Value:
 *   OK, or a negated errno.
 *
 ****************************************************************************/

int softgl_mesh_load_memory(struct softgl_mesh_s *mesh, const void *data,
                            size_t len)
{
  const struct softgl_mesh_header_s *hdr;
  const uint8_t *base = (const uint8_t *)data;
  struct softgl_vertex_s *verts;
  uint16_t *indices;
  size_t vbytes;
  size_t ibytes;

  if (len < sizeof(*hdr))
    {
      return -EINVAL;
    }

  hdr = (const struct softgl_mesh_header_s *)base;

  if (hdr->magic != SOFTGL_MESH_MAGIC ||
      hdr->version != SOFTGL_MESH_VERSION)
    {
      return -EINVAL;
    }

  if (hdr->nvertices == 0 || hdr->nvertices > SOFTGL_MESH_MAX_VERTS ||
      hdr->nindices == 0 || hdr->nindices > SOFTGL_MESH_MAX_INDICES ||
      (hdr->nindices % 3) != 0)
    {
      return -EINVAL;
    }

  vbytes = (size_t)hdr->nvertices * sizeof(struct softgl_vertex_s);
  ibytes = (size_t)hdr->nindices * sizeof(uint16_t);

  if (len < sizeof(*hdr) + vbytes + ibytes)
    {
      return -EINVAL;
    }

  verts = (struct softgl_vertex_s *)malloc(vbytes);
  if (verts == NULL)
    {
      return -ENOMEM;
    }

  indices = (uint16_t *)malloc(ibytes);
  if (indices == NULL)
    {
      free(verts);
      return -ENOMEM;
    }

  memcpy(verts, base + sizeof(*hdr), vbytes);
  memcpy(indices, base + sizeof(*hdr) + vbytes, ibytes);

  memset(mesh, 0, sizeof(*mesh));
  mesh->vertices     = verts;
  mesh->indices      = indices;
  mesh->nvertices    = hdr->nvertices;
  mesh->nindices     = hdr->nindices;
  mesh->owned        = true;
  mesh->basecolor[0] = 1.0f;
  mesh->basecolor[1] = 1.0f;
  mesh->basecolor[2] = 1.0f;

  return OK;
}

/****************************************************************************
 * Name: softgl_mesh_load
 *
 * Description:
 *   Load a .mesh container from the filesystem.
 *
 * Returned Value:
 *   OK, or a negated errno.
 *
 ****************************************************************************/

int softgl_mesh_load(struct softgl_mesh_s *mesh, const char *path)
{
  struct stat sb;
  uint8_t *buf;
  ssize_t nread;
  size_t total = 0;
  int fd;
  int ret;

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  if (fstat(fd, &sb) < 0 || sb.st_size <= 0)
    {
      close(fd);
      return -EINVAL;
    }

  buf = (uint8_t *)malloc((size_t)sb.st_size);
  if (buf == NULL)
    {
      close(fd);
      return -ENOMEM;
    }

  while (total < (size_t)sb.st_size)
    {
      nread = read(fd, buf + total, (size_t)sb.st_size - total);
      if (nread <= 0)
        {
          free(buf);
          close(fd);
          return -EIO;
        }

      total += (size_t)nread;
    }

  close(fd);

  ret = softgl_mesh_load_memory(mesh, buf, total);
  free(buf);
  return ret;
}

/****************************************************************************
 * Name: softgl_mesh_load_obj
 *
 * Description:
 *   Minimal Wavefront OBJ reader: v / vt / vn / f.  Polygons with more than
 *   three corners are triangulated as a fan.  Unique v/vt/vn combinations
 *   become unique vertices; the de-duplication is a linear scan over the
 *   emitted vertices bucketed by position index, which is fast enough for
 *   the low-poly assets this renderer targets.  Missing normals are
 *   generated from face geometry and then area-weighted averaged.
 *
 *   For production the offline tools/obj_to_mesh.py converter plus
 *   softgl_mesh_load() is preferred; this parser exists so that assets can
 *   be iterated on directly from the board's /data partition.
 *
 * Returned Value:
 *   OK, or a negated errno.
 *
 ****************************************************************************/

int softgl_mesh_load_obj(struct softgl_mesh_s *mesh, const char *path)
{
  FILE *fp;
  char line[SOFTGL_OBJ_LINE_MAX];
  float *pos = NULL;
  float *nrm = NULL;
  float *uv  = NULL;
  uint32_t poscap = 0;
  uint32_t nrmcap = 0;
  uint32_t uvcap  = 0;
  uint32_t npos = 0;
  uint32_t nnrm = 0;
  uint32_t nuv  = 0;
  struct softgl_vertex_s *verts = NULL;
  uint16_t *indices = NULL;
  struct softgl_objkey_s *keys = NULL;   /* v/vt/vn triple per vertex */
  uint32_t vcap = 0;
  uint32_t icap = 0;
  uint32_t kcap = 0;
  uint32_t nverts = 0;
  uint32_t nidx = 0;
  bool have_normals = false;
  int ret = OK;

  fp = fopen(path, "r");
  if (fp == NULL)
    {
      return -errno;
    }

  while (fgets(line, sizeof(line), fp) != NULL)
    {
      if (line[0] == 'v' && line[1] == ' ')
        {
          float x;
          float y;
          float z;

          if (sscanf(line + 2, "%f %f %f", &x, &y, &z) != 3)
            {
              continue;
            }

          ret = softgl_obj_reserve((void **)&pos, &poscap, (npos + 1) * 3,
                                   sizeof(float));
          if (ret < 0)
            {
              goto errout;
            }

          pos[npos * 3 + 0] = x;
          pos[npos * 3 + 1] = y;
          pos[npos * 3 + 2] = z;
          npos++;
        }
      else if (line[0] == 'v' && line[1] == 'n')
        {
          float x;
          float y;
          float z;

          if (sscanf(line + 3, "%f %f %f", &x, &y, &z) != 3)
            {
              continue;
            }

          ret = softgl_obj_reserve((void **)&nrm, &nrmcap, (nnrm + 1) * 3,
                                   sizeof(float));
          if (ret < 0)
            {
              goto errout;
            }

          nrm[nnrm * 3 + 0] = x;
          nrm[nnrm * 3 + 1] = y;
          nrm[nnrm * 3 + 2] = z;
          nnrm++;
          have_normals = true;
        }
      else if (line[0] == 'v' && line[1] == 't')
        {
          float u;
          float v;

          if (sscanf(line + 3, "%f %f", &u, &v) != 2)
            {
              continue;
            }

          ret = softgl_obj_reserve((void **)&uv, &uvcap, (nuv + 1) * 2,
                                   sizeof(float));
          if (ret < 0)
            {
              goto errout;
            }

          uv[nuv * 2 + 0] = u;
          uv[nuv * 2 + 1] = v;
          nuv++;
        }
      else if (line[0] == 'f' && line[1] == ' ')
        {
          uint16_t fan[3];
          int corner = 0;
          char *cursor = line + 2;

          while (*cursor != '\0')
            {
              int vi = 0;
              int ti = 0;
              int ni = 0;
              uint32_t k;
              uint16_t emitted;

              while (*cursor == ' ' || *cursor == '\t')
                {
                  cursor++;
                }

              if (*cursor == '\0' || *cursor == '\n' || *cursor == '\r')
                {
                  break;
                }

              /* Accept v, v/vt, v//vn and v/vt/vn. */

              vi = (int)strtol(cursor, &cursor, 10);
              if (*cursor == '/')
                {
                  cursor++;
                  if (*cursor != '/')
                    {
                      ti = (int)strtol(cursor, &cursor, 10);
                    }

                  if (*cursor == '/')
                    {
                      cursor++;
                      ni = (int)strtol(cursor, &cursor, 10);
                    }
                }

              /* OBJ indices are 1-based; negative values are relative. */

              vi = vi > 0 ? vi - 1 : (int)npos + vi;
              ti = ti > 0 ? ti - 1 : (ti < 0 ? (int)nuv + ti : -1);
              ni = ni > 0 ? ni - 1 : (ni < 0 ? (int)nnrm + ni : -1);

              if (vi < 0 || (uint32_t)vi >= npos)
                {
                  ret = -EINVAL;
                  goto errout;
                }

              /* Look for an identical v/vt/vn triple already emitted. */

              emitted = 0xffff;
              for (k = 0; k < nverts; k++)
                {
                  if (keys[k].v == vi && keys[k].t == ti &&
                      keys[k].n == ni)
                    {
                      emitted = (uint16_t)k;
                      break;
                    }
                }

              if (emitted == 0xffff)
                {
                  struct softgl_vertex_s *nv;

                  if (nverts >= SOFTGL_MESH_MAX_VERTS)
                    {
                      ret = -E2BIG;
                      goto errout;
                    }

                  ret = softgl_obj_reserve((void **)&verts, &vcap,
                                           nverts + 1,
                                           sizeof(struct softgl_vertex_s));
                  if (ret < 0)
                    {
                      goto errout;
                    }

                  ret = softgl_obj_reserve((void **)&keys, &kcap,
                                           nverts + 1,
                                           sizeof(struct softgl_objkey_s));
                  if (ret < 0)
                    {
                      goto errout;
                    }

                  nv = &verts[nverts];
                  memset(nv, 0, sizeof(*nv));
                  nv->pos[0] = pos[vi * 3 + 0];
                  nv->pos[1] = pos[vi * 3 + 1];
                  nv->pos[2] = pos[vi * 3 + 2];

                  if (ni >= 0 && (uint32_t)ni < nnrm)
                    {
                      nv->nrm[0] = nrm[ni * 3 + 0];
                      nv->nrm[1] = nrm[ni * 3 + 1];
                      nv->nrm[2] = nrm[ni * 3 + 2];
                    }

                  if (ti >= 0 && (uint32_t)ti < nuv)
                    {
                      nv->uv[0] = uv[ti * 2 + 0];

                      /* OBJ's V axis points up, ours points down. */

                      nv->uv[1] = 1.0f - uv[ti * 2 + 1];
                    }

                  keys[nverts].v = vi;
                  keys[nverts].t = ti;
                  keys[nverts].n = ni;
                  emitted = (uint16_t)nverts;
                  nverts++;
                }

              if (corner < 2)
                {
                  fan[corner] = emitted;
                  corner++;
                  continue;
                }

              fan[2] = emitted;

              ret = softgl_obj_reserve((void **)&indices, &icap, nidx + 3,
                                       sizeof(uint16_t));
              if (ret < 0)
                {
                  goto errout;
                }

              indices[nidx++] = fan[0];
              indices[nidx++] = fan[1];
              indices[nidx++] = fan[2];

              /* Next triangle of the fan reuses corner 0 and this corner. */

              fan[1] = emitted;
              corner++;
            }
        }
    }

  fclose(fp);
  fp = NULL;

  if (nverts == 0 || nidx == 0)
    {
      ret = -EINVAL;
      goto errout;
    }

  /* Synthesise smooth normals when the file carried none. */

  if (!have_normals)
    {
      uint32_t i;

      for (i = 0; i < nidx; i += 3)
        {
          struct softgl_vertex_s *a = &verts[indices[i + 0]];
          struct softgl_vertex_s *b = &verts[indices[i + 1]];
          struct softgl_vertex_s *c = &verts[indices[i + 2]];
          struct softgl_vec3_s ab = softgl_vec3(b->pos[0] - a->pos[0],
                                                b->pos[1] - a->pos[1],
                                                b->pos[2] - a->pos[2]);
          struct softgl_vec3_s ac = softgl_vec3(c->pos[0] - a->pos[0],
                                                c->pos[1] - a->pos[1],
                                                c->pos[2] - a->pos[2]);

          /* Un-normalised cross product is area weighted, which is what we
           * want when accumulating.
           */

          struct softgl_vec3_s n = softgl_vec3_cross(ab, ac);

          a->nrm[0] += n.x; a->nrm[1] += n.y; a->nrm[2] += n.z;
          b->nrm[0] += n.x; b->nrm[1] += n.y; b->nrm[2] += n.z;
          c->nrm[0] += n.x; c->nrm[1] += n.y; c->nrm[2] += n.z;
        }

      for (i = 0; i < nverts; i++)
        {
          struct softgl_vec3_s n = softgl_vec3_normalize(
            softgl_vec3(verts[i].nrm[0], verts[i].nrm[1], verts[i].nrm[2]));

          verts[i].nrm[0] = n.x;
          verts[i].nrm[1] = n.y;
          verts[i].nrm[2] = n.z;
        }
    }

  free(pos);
  free(nrm);
  free(uv);
  free(keys);

  memset(mesh, 0, sizeof(*mesh));
  mesh->vertices     = verts;
  mesh->indices      = indices;
  mesh->nvertices    = nverts;
  mesh->nindices     = nidx;
  mesh->owned        = true;
  mesh->basecolor[0] = 1.0f;
  mesh->basecolor[1] = 1.0f;
  mesh->basecolor[2] = 1.0f;

  return OK;

errout:
  if (fp != NULL)
    {
      fclose(fp);
    }

  free(pos);
  free(nrm);
  free(uv);
  free(verts);
  free(indices);
  free(keys);
  return ret;
}

/****************************************************************************
 * Name: softgl_mesh_free
 ****************************************************************************/

void softgl_mesh_free(struct softgl_mesh_s *mesh)
{
  if (mesh == NULL)
    {
      return;
    }

  if (mesh->owned)
    {
      free((void *)mesh->vertices);
      free((void *)mesh->indices);
    }

  memset(mesh, 0, sizeof(*mesh));
}

/****************************************************************************
 * Name: softgl_texture_free
 ****************************************************************************/

void softgl_texture_free(struct softgl_texture_s *tex)
{
  if (tex == NULL)
    {
      return;
    }

  if (tex->owned)
    {
      free((void *)tex->pixels);
    }

  memset(tex, 0, sizeof(*tex));
}
