/****************************************************************************
 * app/softgl/softgl_raster.c
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
 * SoftGL rasteriser.
 *
 * A draw call runs in two phases:
 *
 *   1. Geometry (single threaded).  Every vertex is transformed once by the
 *      MVP matrix (NEON), then each triangle is clipped against the near
 *      plane, projected, back-face culled and binned into a flat array of
 *      screen-space triangles.  Varyings are pre-divided by w here so the
 *      inner loop only has to interpolate linearly.
 *
 *   2. Scan conversion (multi threaded).  The framebuffer is cut into
 *      horizontal bands, one per thread.  Every thread walks the whole
 *      triangle bin but clips each triangle's bounding box to its own band,
 *      so the colour and depth buffers are partitioned by construction and
 *      the inner loop needs no synchronisation at all.
 *
 * Coverage uses the half-space (edge function) test with incremental
 * evaluation along X and a top-left fill rule, so shared edges are rendered
 * exactly once.  Depth is a 16-bit window-space value tested before shading
 * (early-Z), which is what keeps the overdraw cost of a closed mesh low.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "softgl.h"

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* A triangle whose absolute screen area is below this is degenerate. */

#define SOFTGL_AREA_EPSILON 1e-6f

/* Sub-pixel bias applied to edges that are neither top nor left, so that a
 * pixel centre lying exactly on a shared edge belongs to one triangle only.
 */

#define SOFTGL_EDGE_BIAS (-1.0f / 256.0f)

/* Clip-space guard: vertices closer than this to the eye are clipped. */

#define SOFTGL_NEAR_EPSILON 1e-5f

/* Sutherland-Hodgman against one plane turns 3 vertices into at most 4. */

#define SOFTGL_CLIP_MAX 4

/* Largest Blinn-Phong exponent we evaluate by repeated squaring. */

#define SOFTGL_SHININESS_MAX 256

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static inline float softgl_clampf(float v, float lo, float hi);
static inline float softgl_fast_pow(float base, int exponent);
static inline uint16_t softgl_pack565(float r, float g, float b);
static inline void softgl_unpack565(uint16_t c, float *r, float *g, float *b);
static void softgl_tex_sample(const struct softgl_texture_s *tex,
                              enum softgl_filter_e filter, float u, float v,
                              float *r, float *g, float *b);
static struct softgl_vsout_s softgl_vs_lerp(const struct softgl_vsout_s *a,
                                            const struct softgl_vsout_s *b,
                                            float t);
static int softgl_clip_near(const struct softgl_vsout_s *in,
                            struct softgl_vsout_s *out);
static int softgl_bin_triangle(struct softgl_context_s *ctx,
                               const struct softgl_vsout_s *v,
                               const struct softgl_mesh_s *mesh);
static void softgl_band_range(struct softgl_context_s *ctx, int band, int *y0,
                              int *y1);
static int softgl_reserve_vsbuf(struct softgl_context_s *ctx, uint32_t count);
static int softgl_reserve_tribin(struct softgl_context_s *ctx);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: softgl_clampf
 ****************************************************************************/

static inline float softgl_clampf(float v, float lo, float hi)
{
  if (v < lo)
    {
      return lo;
    }

  if (v > hi)
    {
      return hi;
    }

  return v;
}

/****************************************************************************
 * Name: softgl_fast_pow
 *
 * Description:
 *   base raised to a small non-negative integer power by repeated squaring.
 *   The specular term is evaluated per pixel, where libm powf() would
 *   dominate the frame time; the exponent is a material constant so an
 *   integer approximation is visually indistinguishable.
 *
 ****************************************************************************/

static inline float softgl_fast_pow(float base, int exponent)
{
  float result = 1.0f;

  while (exponent > 0)
    {
      if ((exponent & 1) != 0)
        {
          result *= base;
        }

      base *= base;
      exponent >>= 1;
    }

  return result;
}

/****************************************************************************
 * Name: softgl_pack565 / softgl_unpack565
 ****************************************************************************/

static inline uint16_t softgl_pack565(float r, float g, float b)
{
  uint32_t ri = (uint32_t)(softgl_clampf(r, 0.0f, 1.0f) * 31.0f + 0.5f);
  uint32_t gi = (uint32_t)(softgl_clampf(g, 0.0f, 1.0f) * 63.0f + 0.5f);
  uint32_t bi = (uint32_t)(softgl_clampf(b, 0.0f, 1.0f) * 31.0f + 0.5f);

  return (uint16_t)((ri << 11) | (gi << 5) | bi);
}

static inline void softgl_unpack565(uint16_t c, float *r, float *g, float *b)
{
  *r = (float)((c >> 11) & 0x1f) * (1.0f / 31.0f);
  *g = (float)((c >> 5) & 0x3f) * (1.0f / 63.0f);
  *b = (float)(c & 0x1f) * (1.0f / 31.0f);
}

/****************************************************************************
 * Name: softgl_tex_sample
 *
 * Description:
 *   Sample an RGB565 texture with repeat wrapping, either nearest neighbour
 *   or bilinear.
 *
 ****************************************************************************/

static void softgl_tex_sample(const struct softgl_texture_s *tex,
                              enum softgl_filter_e filter, float u, float v,
                              float *r, float *g, float *b)
{
  int w = tex->width;
  int h = tex->height;

  /* Repeat wrap: floorf() keeps negative UVs well defined. */

  u -= floorf(u);
  v -= floorf(v);

  if (filter == SOFTGL_FILTER_NEAREST)
    {
      int x = (int)(u * (float)w);
      int y = (int)(v * (float)h);

      if (x >= w)
        {
          x = w - 1;
        }

      if (y >= h)
        {
          y = h - 1;
        }

      softgl_unpack565(tex->pixels[(size_t)y * w + x], r, g, b);
      return;
    }

  /* Bilinear: sample at texel centres and blend the 2x2 neighbourhood. */

  {
    float fx = u * (float)w - 0.5f;
    float fy = v * (float)h - 0.5f;
    int x0 = (int)floorf(fx);
    int y0 = (int)floorf(fy);
    float tx = fx - (float)x0;
    float ty = fy - (float)y0;
    int x1;
    int y1;
    float r00;
    float g00;
    float b00;
    float r10;
    float g10;
    float b10;
    float r01;
    float g01;
    float b01;
    float r11;
    float g11;
    float b11;
    float w00;
    float w10;
    float w01;
    float w11;

    x0 = ((x0 % w) + w) % w;
    y0 = ((y0 % h) + h) % h;
    x1 = (x0 + 1) % w;
    y1 = (y0 + 1) % h;

    softgl_unpack565(tex->pixels[(size_t)y0 * w + x0], &r00, &g00, &b00);
    softgl_unpack565(tex->pixels[(size_t)y0 * w + x1], &r10, &g10, &b10);
    softgl_unpack565(tex->pixels[(size_t)y1 * w + x0], &r01, &g01, &b01);
    softgl_unpack565(tex->pixels[(size_t)y1 * w + x1], &r11, &g11, &b11);

    w00 = (1.0f - tx) * (1.0f - ty);
    w10 = tx * (1.0f - ty);
    w01 = (1.0f - tx) * ty;
    w11 = tx * ty;

    *r = r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11;
    *g = g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11;
    *b = b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11;
  }
}

/****************************************************************************
 * Name: softgl_vs_lerp
 *
 * Description:
 *   Linear interpolation of a post-vertex-shader vertex, used by the near
 *   plane clipper.  Clip space is still linear in the original vertex
 *   attributes, so plain lerp is correct here.
 *
 ****************************************************************************/

static struct softgl_vsout_s softgl_vs_lerp(const struct softgl_vsout_s *a,
                                            const struct softgl_vsout_s *b,
                                            float t)
{
  struct softgl_vsout_s r;

  r.clip.x = a->clip.x + (b->clip.x - a->clip.x) * t;
  r.clip.y = a->clip.y + (b->clip.y - a->clip.y) * t;
  r.clip.z = a->clip.z + (b->clip.z - a->clip.z) * t;
  r.clip.w = a->clip.w + (b->clip.w - a->clip.w) * t;

  r.wpos.x = a->wpos.x + (b->wpos.x - a->wpos.x) * t;
  r.wpos.y = a->wpos.y + (b->wpos.y - a->wpos.y) * t;
  r.wpos.z = a->wpos.z + (b->wpos.z - a->wpos.z) * t;

  r.wnrm.x = a->wnrm.x + (b->wnrm.x - a->wnrm.x) * t;
  r.wnrm.y = a->wnrm.y + (b->wnrm.y - a->wnrm.y) * t;
  r.wnrm.z = a->wnrm.z + (b->wnrm.z - a->wnrm.z) * t;

  r.u = a->u + (b->u - a->u) * t;
  r.v = a->v + (b->v - a->v) * t;

  return r;
}

/****************************************************************************
 * Name: softgl_clip_near
 *
 * Description:
 *   Sutherland-Hodgman clip of one triangle against the near plane
 *   z + w > 0 (OpenGL convention).  This is the only plane that must be
 *   clipped: the four side planes and the far plane are handled by the
 *   screen-space bounding box and the depth clamp respectively, which is
 *   both cheaper and numerically safer.
 *
 * Input Parameters:
 *   in  - three clip-space vertices
 *   out - receives up to SOFTGL_CLIP_MAX vertices, in fan order
 *
 * Returned Value:
 *   Number of vertices in out (0, 3 or 4).
 *
 ****************************************************************************/

static int softgl_clip_near(const struct softgl_vsout_s *in,
                            struct softgl_vsout_s *out)
{
  float dist[3];
  int nout = 0;
  int i;

  for (i = 0; i < 3; i++)
    {
      dist[i] = in[i].clip.z + in[i].clip.w;
    }

  if (dist[0] > SOFTGL_NEAR_EPSILON && dist[1] > SOFTGL_NEAR_EPSILON &&
      dist[2] > SOFTGL_NEAR_EPSILON)
    {
      out[0] = in[0];
      out[1] = in[1];
      out[2] = in[2];
      return 3;
    }

  if (dist[0] <= SOFTGL_NEAR_EPSILON && dist[1] <= SOFTGL_NEAR_EPSILON &&
      dist[2] <= SOFTGL_NEAR_EPSILON)
    {
      return 0;
    }

  for (i = 0; i < 3; i++)
    {
      int j = (i + 1) % 3;
      bool in_i = dist[i] > SOFTGL_NEAR_EPSILON;
      bool in_j = dist[j] > SOFTGL_NEAR_EPSILON;

      if (in_i)
        {
          out[nout++] = in[i];
        }

      if (in_i != in_j)
        {
          float t = dist[i] / (dist[i] - dist[j]);

          out[nout++] = softgl_vs_lerp(&in[i], &in[j], t);
        }
    }

  return nout;
}

/****************************************************************************
 * Name: softgl_reserve_vsbuf
 ****************************************************************************/

static int softgl_reserve_vsbuf(struct softgl_context_s *ctx, uint32_t count)
{
  struct softgl_vsout_s *p;

  if (count <= ctx->vscap)
    {
      return OK;
    }

  p = (struct softgl_vsout_s *)realloc(ctx->vsbuf, (size_t)count * sizeof(*p));
  if (p == NULL)
    {
      return -ENOMEM;
    }

  ctx->vsbuf = p;
  ctx->vscap = count;
  return OK;
}

/****************************************************************************
 * Name: softgl_reserve_tribin
 ****************************************************************************/

static int softgl_reserve_tribin(struct softgl_context_s *ctx)
{
  struct softgl_rtri_s *p;
  uint32_t newcap;

  if (ctx->ntris < ctx->tricap)
    {
      return OK;
    }

  newcap = ctx->tricap != 0 ? ctx->tricap * 2 : 1024;
  p = (struct softgl_rtri_s *)realloc(ctx->tris, (size_t)newcap * sizeof(*p));
  if (p == NULL)
    {
      return -ENOMEM;
    }

  ctx->tris = p;
  ctx->tricap = newcap;
  return OK;
}

/****************************************************************************
 * Name: softgl_bin_triangle
 *
 * Description:
 *   Project one clipped triangle to screen space, cull it, and append it to
 *   the triangle bin.  Runs single threaded between the vertex stage and
 *   the band dispatch.
 *
 * Returned Value:
 *   1 if the triangle was binned, 0 if it was culled, negated errno on
 *   allocation failure.
 *
 ****************************************************************************/

static int softgl_bin_triangle(struct softgl_context_s *ctx,
                               const struct softgl_vsout_s *v,
                               const struct softgl_mesh_s *mesh)
{
  struct softgl_rtri_s *t;
  float sx[3];
  float sy[3];
  float sz[3];
  float iw[3];
  const struct softgl_vsout_s *src[3];
  float area;
  float swap;
  float hw = (float)ctx->width * 0.5f;
  float hh = (float)ctx->height * 0.5f;
  float minx;
  float maxx;
  float miny;
  float maxy;
  int i;

  for (i = 0; i < 3; i++)
    {
      float w = v[i].clip.w;

      if (w <= SOFTGL_NEAR_EPSILON)
        {
          /* Should not survive the near clip, but never divide by ~0. */

          return 0;
        }

      iw[i] = 1.0f / w;

      /* NDC then viewport.  Y is flipped because screen rows grow down. */

      sx[i] = (v[i].clip.x * iw[i] + 1.0f) * hw;
      sy[i] = (1.0f - v[i].clip.y * iw[i]) * hh;
      sz[i] = softgl_clampf(v[i].clip.z * iw[i] * 0.5f + 0.5f, 0.0f, 1.0f);
    }

  /* Signed area of the screen-space triangle.  With the Y flip a front
   * facing (counter-clockwise in NDC) triangle comes out negative.
   */

  area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);

  if (area > -SOFTGL_AREA_EPSILON && area < SOFTGL_AREA_EPSILON)
    {
      return 0;
    }

  if (ctx->cull == SOFTGL_CULL_BACK && area > 0.0f)
    {
      return 0;
    }

  if (ctx->cull == SOFTGL_CULL_FRONT && area < 0.0f)
    {
      return 0;
    }

  src[0] = &v[0];

  if (area < 0.0f)
    {
      /* Normalise the winding so the edge functions are positive inside. */

      src[1] = &v[2];
      src[2] = &v[1];

      swap = sx[1];
      sx[1] = sx[2];
      sx[2] = swap;

      swap = sy[1];
      sy[1] = sy[2];
      sy[2] = swap;

      swap = sz[1];
      sz[1] = sz[2];
      sz[2] = swap;

      swap = iw[1];
      iw[1] = iw[2];
      iw[2] = swap;

      area = -area;
    }
  else
    {
      src[1] = &v[1];
      src[2] = &v[2];
    }

  /* Screen-space bounding box, clamped to the viewport. */

  minx = sx[0] < sx[1] ? (sx[0] < sx[2] ? sx[0] : sx[2])
                       : (sx[1] < sx[2] ? sx[1] : sx[2]);
  maxx = sx[0] > sx[1] ? (sx[0] > sx[2] ? sx[0] : sx[2])
                       : (sx[1] > sx[2] ? sx[1] : sx[2]);
  miny = sy[0] < sy[1] ? (sy[0] < sy[2] ? sy[0] : sy[2])
                       : (sy[1] < sy[2] ? sy[1] : sy[2]);
  maxy = sy[0] > sy[1] ? (sy[0] > sy[2] ? sy[0] : sy[2])
                       : (sy[1] > sy[2] ? sy[1] : sy[2]);

  if (softgl_reserve_tribin(ctx) < 0)
    {
      return -ENOMEM;
    }

  t = &ctx->tris[ctx->ntris];

  t->minx = (int)floorf(minx);
  t->maxx = (int)ceilf(maxx);
  t->miny = (int)floorf(miny);
  t->maxy = (int)ceilf(maxy);

  if (t->minx < 0)
    {
      t->minx = 0;
    }

  if (t->miny < 0)
    {
      t->miny = 0;
    }

  if (t->maxx > ctx->width - 1)
    {
      t->maxx = ctx->width - 1;
    }

  if (t->maxy > ctx->height - 1)
    {
      t->maxy = ctx->height - 1;
    }

  if (t->minx > t->maxx || t->miny > t->maxy)
    {
      return 0;
    }

  for (i = 0; i < 3; i++)
    {
      const struct softgl_vsout_s *s = src[i];
      float q = iw[i];

      t->x[i] = sx[i];
      t->y[i] = sy[i];
      t->z[i] = sz[i];
      t->iw[i] = q;

      /* Pre-divide every varying by w so that screen-space linear
       * interpolation of (attr/w) and (1/w) reconstructs the perspective
       * correct attribute as (attr/w) / (1/w).
       */

      t->var[i][SOFTGL_VARY_U] = s->u * q;
      t->var[i][SOFTGL_VARY_V] = s->v * q;
      t->var[i][SOFTGL_VARY_NX] = s->wnrm.x * q;
      t->var[i][SOFTGL_VARY_NY] = s->wnrm.y * q;
      t->var[i][SOFTGL_VARY_NZ] = s->wnrm.z * q;
      t->var[i][SOFTGL_VARY_WX] = s->wpos.x * q;
      t->var[i][SOFTGL_VARY_WY] = s->wpos.y * q;
      t->var[i][SOFTGL_VARY_WZ] = s->wpos.z * q;
    }

  /* Top-left fill rule.  With a Y-down viewport and positive area, edge
   * (a -> b) owns pixels exactly on it when it is horizontal running right
   * (a top edge) or when it runs upwards (a left edge).
   */

  for (i = 0; i < 3; i++)
    {
      int a = (i + 1) % 3;
      int b = (i + 2) % 3;
      bool topleft =
          (t->y[a] == t->y[b] && t->x[b] > t->x[a]) || (t->y[b] < t->y[a]);

      t->bias[i] = topleft ? 0.0f : SOFTGL_EDGE_BIAS;
    }

  t->inv_area = 1.0f / area;
  t->tex = mesh->texture != NULL ? mesh->texture : ctx->texture;

  t->base[0] = mesh->basecolor[0];
  t->base[1] = mesh->basecolor[1];
  t->base[2] = mesh->basecolor[2];

  ctx->ntris++;
  return 1;
}

/****************************************************************************
 * Name: softgl_band_range
 *
 * Description:
 *   Compute the half-open scanline range [y0, y1) owned by one band.
 *
 ****************************************************************************/

static void softgl_band_range(struct softgl_context_s *ctx, int band, int *y0,
                              int *y1)
{
  int nbands = ctx->nbands > 0 ? ctx->nbands : 1;
  int rows_per;

  rows_per = (ctx->height + nbands - 1) / nbands;
  rows_per = ((rows_per + SOFTGL_BAND_ALIGN - 1) / SOFTGL_BAND_ALIGN) *
             SOFTGL_BAND_ALIGN;

  *y0 = band * rows_per;
  if (*y0 > ctx->height)
    {
      *y0 = ctx->height;
    }

  *y1 = *y0 + rows_per;
  if (*y1 > ctx->height)
    {
      *y1 = ctx->height;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: softgl_raster_band
 *
 * Description:
 *   Scan convert the whole triangle bin, restricted to the scanlines owned
 *   by "band".  Called once per band per draw call, concurrently.
 *
 ****************************************************************************/

void softgl_raster_band(struct softgl_context_s *ctx, int band)
{
  struct softgl_vec3_s lightdir = ctx->light.direction;
  struct softgl_vec3_s lightcol = ctx->light.color;
  struct softgl_vec3_s ambient = ctx->light.ambient;
  enum softgl_shade_e shade = ctx->shade;
  enum softgl_filter_e filter = ctx->filter;
  float spec_strength = ctx->light.specular;
  int shininess = (int)ctx->light.shininess;
  uint32_t pixels = 0;
  int band_y0;
  int band_y1;
  uint32_t ti;

  if (shininess < 1)
    {
      shininess = 1;
    }
  else if (shininess > SOFTGL_SHININESS_MAX)
    {
      shininess = SOFTGL_SHININESS_MAX;
    }

  softgl_band_range(ctx, band, &band_y0, &band_y1);
  if (band_y0 >= band_y1)
    {
      return;
    }

  for (ti = 0; ti < ctx->ntris; ti++)
    {
      const struct softgl_rtri_s *t = &ctx->tris[ti];
      float e0dx;
      float e1dx;
      float e2dx;
      int y0;
      int y1;
      int y;

      y0 = t->miny > band_y0 ? t->miny : band_y0;
      y1 = t->maxy < band_y1 - 1 ? t->maxy : band_y1 - 1;

      if (y0 > y1)
        {
          continue;
        }

      /* X derivative of each edge function.  e_i is the edge opposite
       * vertex i, i.e. the barycentric weight of vertex i.  The row start
       * value is evaluated directly once per scanline, which is cheaper
       * than carrying a Y accumulator across a clipped band.
       */

      e0dx = t->y[1] - t->y[2];
      e1dx = t->y[2] - t->y[0];
      e2dx = t->y[0] - t->y[1];

      for (y = y0; y <= y1; y++)
        {
          float px = (float)t->minx + 0.5f;
          float py = (float)y + 0.5f;
          float e0;
          float e1;
          float e2;
          uint16_t *crow;
          uint16_t *drow;
          int x;

          e0 = (t->x[2] - t->x[1]) * (py - t->y[1]) -
               (t->y[2] - t->y[1]) * (px - t->x[1]);
          e1 = (t->x[0] - t->x[2]) * (py - t->y[2]) -
               (t->y[0] - t->y[2]) * (px - t->x[2]);
          e2 = (t->x[1] - t->x[0]) * (py - t->y[0]) -
               (t->y[1] - t->y[0]) * (px - t->x[0]);

          crow = ctx->color + (size_t)y * ctx->stride;
          drow = ctx->depth + (size_t)y * ctx->width;

          for (x = t->minx; x <= t->maxx;
               x++, e0 += e0dx, e1 += e1dx, e2 += e2dx)
            {
              float b0;
              float b1;
              float b2;
              float zf;
              uint32_t zi;
              float iw;
              float w;
              float r;
              float g;
              float b;

              if (e0 + t->bias[0] < 0.0f || e1 + t->bias[1] < 0.0f ||
                  e2 + t->bias[2] < 0.0f)
                {
                  continue;
                }

              b0 = e0 * t->inv_area;
              b1 = e1 * t->inv_area;
              b2 = e2 * t->inv_area;

              /* Window-space depth is linear in screen space, so it can be
               * interpolated without the perspective correction.
               */

              zf = b0 * t->z[0] + b1 * t->z[1] + b2 * t->z[2];
              zi = (uint32_t)(softgl_clampf(zf, 0.0f, 1.0f) *
                              (float)SOFTGL_DEPTH_MAX);

              /* Early-Z: reject before doing any shading work. */

              if (ctx->depth_test && zi >= drow[x])
                {
                  continue;
                }

              iw = b0 * t->iw[0] + b1 * t->iw[1] + b2 * t->iw[2];
              if (iw <= 0.0f)
                {
                  continue;
                }

              w = 1.0f / iw;

              r = t->base[0];
              g = t->base[1];
              b = t->base[2];

              if (t->tex != NULL)
                {
                  float u = (b0 * t->var[0][SOFTGL_VARY_U] +
                             b1 * t->var[1][SOFTGL_VARY_U] +
                             b2 * t->var[2][SOFTGL_VARY_U]) *
                            w;
                  float v = (b0 * t->var[0][SOFTGL_VARY_V] +
                             b1 * t->var[1][SOFTGL_VARY_V] +
                             b2 * t->var[2][SOFTGL_VARY_V]) *
                            w;
                  float tr;
                  float tg;
                  float tb;

                  softgl_tex_sample(t->tex, filter, u, v, &tr, &tg, &tb);
                  r *= tr;
                  g *= tg;
                  b *= tb;
                }

              if (shade != SOFTGL_SHADE_UNLIT)
                {
                  struct softgl_vec3_s n;
                  float ndotl;
                  float diff;
                  float spec = 0.0f;

                  n.x = (b0 * t->var[0][SOFTGL_VARY_NX] +
                         b1 * t->var[1][SOFTGL_VARY_NX] +
                         b2 * t->var[2][SOFTGL_VARY_NX]) *
                        w;
                  n.y = (b0 * t->var[0][SOFTGL_VARY_NY] +
                         b1 * t->var[1][SOFTGL_VARY_NY] +
                         b2 * t->var[2][SOFTGL_VARY_NY]) *
                        w;
                  n.z = (b0 * t->var[0][SOFTGL_VARY_NZ] +
                         b1 * t->var[1][SOFTGL_VARY_NZ] +
                         b2 * t->var[2][SOFTGL_VARY_NZ]) *
                        w;
                  n = softgl_vec3_normalize(n);

                  ndotl = softgl_vec3_dot(n, lightdir);
                  diff = ndotl > 0.0f ? ndotl : 0.0f;

                  if (shade == SOFTGL_SHADE_PHONG && diff > 0.0f &&
                      spec_strength > 0.0f)
                    {
                      struct softgl_vec3_s wpos;
                      struct softgl_vec3_s view;
                      struct softgl_vec3_s half;
                      float ndoth;

                      wpos.x = (b0 * t->var[0][SOFTGL_VARY_WX] +
                                b1 * t->var[1][SOFTGL_VARY_WX] +
                                b2 * t->var[2][SOFTGL_VARY_WX]) *
                               w;
                      wpos.y = (b0 * t->var[0][SOFTGL_VARY_WY] +
                                b1 * t->var[1][SOFTGL_VARY_WY] +
                                b2 * t->var[2][SOFTGL_VARY_WY]) *
                               w;
                      wpos.z = (b0 * t->var[0][SOFTGL_VARY_WZ] +
                                b1 * t->var[1][SOFTGL_VARY_WZ] +
                                b2 * t->var[2][SOFTGL_VARY_WZ]) *
                               w;

                      view = softgl_vec3_normalize(
                          softgl_vec3_sub(ctx->eye, wpos));
                      half = softgl_vec3_normalize(
                          softgl_vec3_add(view, lightdir));

                      ndoth = softgl_vec3_dot(n, half);
                      if (ndoth > 0.0f)
                        {
                          spec = softgl_fast_pow(ndoth, shininess) *
                                 spec_strength;
                        }
                    }

                  /* Albedo is modulated by ambient plus diffuse; the
                   * specular highlight is added on top untinted.
                   */

                  r = r * (ambient.x + lightcol.x * diff) + lightcol.x * spec;
                  g = g * (ambient.y + lightcol.y * diff) + lightcol.y * spec;
                  b = b * (ambient.z + lightcol.z * diff) + lightcol.z * spec;
                }

              crow[x] = softgl_pack565(r, g, b);

              if (ctx->depth_write)
                {
                  drow[x] = (uint16_t)zi;
                }

              pixels++;
            }
        }
    }

  /* Statistics are advisory; band 0 owns the counter to avoid a race. */

  if (band == 0)
    {
      ctx->stat_pixels += pixels;
    }
}

/****************************************************************************
 * Name: softgl_draw_mesh
 *
 * Description:
 *   Run the full pipeline for one indexed triangle mesh with the context's
 *   current matrices, texture, lighting and cull state.
 *
 * Returned Value:
 *   The number of triangles that reached the rasteriser, or a negated
 *   errno on failure.
 *
 ****************************************************************************/

int softgl_draw_mesh(struct softgl_context_s *ctx,
                     const struct softgl_mesh_s *mesh)
{
  uint32_t i;
  int drawn = 0;

  if (ctx == NULL || mesh == NULL || mesh->nindices < 3 ||
      mesh->vertices == NULL || mesh->indices == NULL)
    {
      return -EINVAL;
    }

  softgl_sync_state(ctx);

  if (softgl_reserve_vsbuf(ctx, mesh->nvertices) < 0)
    {
      return -ENOMEM;
    }

  /* ---- Vertex stage --------------------------------------------------- */

  for (i = 0; i < mesh->nvertices; i++)
    {
      const struct softgl_vertex_s *src = &mesh->vertices[i];
      struct softgl_vsout_s *dst = &ctx->vsbuf[i];
      struct softgl_vec3_s pos =
          softgl_vec3(src->pos[0], src->pos[1], src->pos[2]);
      struct softgl_vec3_s nrm =
          softgl_vec3(src->nrm[0], src->nrm[1], src->nrm[2]);

      dst->clip = softgl_mat4_mul_vec4(&ctx->mvp,
                                       softgl_vec4(pos.x, pos.y, pos.z, 1.0f));
      dst->wpos = softgl_mat4_mul_point(&ctx->model, pos);
      dst->wnrm = softgl_mat4_mul_dir(&ctx->normalmat, nrm);
      dst->u = src->uv[0];
      dst->v = src->uv[1];
    }

  /* ---- Clip, project and bin ------------------------------------------ */

  ctx->ntris = 0;

  for (i = 0; i + 2 < mesh->nindices; i += 3)
    {
      struct softgl_vsout_s tri[3];
      struct softgl_vsout_s poly[SOFTGL_CLIP_MAX];
      int npoly;
      int k;

      if (mesh->indices[i] >= mesh->nvertices ||
          mesh->indices[i + 1] >= mesh->nvertices ||
          mesh->indices[i + 2] >= mesh->nvertices)
        {
          return -EINVAL;
        }

      tri[0] = ctx->vsbuf[mesh->indices[i]];
      tri[1] = ctx->vsbuf[mesh->indices[i + 1]];
      tri[2] = ctx->vsbuf[mesh->indices[i + 2]];

      ctx->stat_tris_in++;

      npoly = softgl_clip_near(tri, poly);

      /* Fan-triangulate the clipped polygon. */

      for (k = 2; k < npoly; k++)
        {
          struct softgl_vsout_s fan[3];
          int ret;

          fan[0] = poly[0];
          fan[1] = poly[k - 1];
          fan[2] = poly[k];

          ret = softgl_bin_triangle(ctx, fan, mesh);
          if (ret < 0)
            {
              return ret;
            }

          drawn += ret;
        }
    }

  ctx->stat_tris_drawn += (uint32_t)drawn;

  /* ---- Scan conversion ------------------------------------------------ */

  if (ctx->ntris > 0)
    {
      softgl_dispatch_bands(ctx);
    }

  return drawn;
}
