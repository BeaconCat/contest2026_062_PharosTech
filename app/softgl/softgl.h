/****************************************************************************
 * app/softgl/softgl.h
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
 * SoftGL: a pure-CPU 3D software rasteriser for openvela / NuttX.
 *
 * Targets the Nyabula 360x360 round displays with no GPU dependency at all.
 * Feature set:
 *
 *   - Fixed-function pipeline: model -> view -> projection -> perspective
 *     divide -> viewport, near-plane clipping, back-face culling.
 *   - Half-space (edge function) triangle rasteriser with a top-left fill
 *     rule and a 16-bit Z-buffer (early-Z reject before shading).
 *   - Perspective-correct interpolation of UV, world normal and world
 *     position.
 *   - RGB565 texture sampling, nearest or bilinear.
 *   - Per-pixel lighting: ambient + Lambert diffuse + Blinn-Phong specular.
 *   - Multi-threaded "sort-middle" rasterisation: the framebuffer is split
 *     into horizontal bands, one band per hardware thread, so no locking is
 *     needed on the colour or depth buffers.
 *   - AArch64 NEON fast paths for the 4x4 matrix/vector product and for
 *     colour/depth buffer clears.
 *
 * Colour buffer format is RGB565 (matches both the ST77916 SPI panels and
 * the usual NuttX /dev/fb0 configuration on this board).
 ****************************************************************************/

#ifndef __APP_SOFTGL_SOFTGL_H
#define __APP_SOFTGL_SOFTGL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pthread.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Maximum number of rasteriser bands (== worker threads + the caller). */

#define SOFTGL_MAX_THREADS        8

/* Band height granularity, in scanlines.  Bands are aligned to this so
 * that each worker touches whole cache-line rows of the colour buffer.
 */

#define SOFTGL_BAND_ALIGN         8

/* Number of perspective-corrected varyings carried per vertex:
 * u, v, nx, ny, nz, wx, wy, wz.
 */

#define SOFTGL_NUM_VARYINGS       8

#define SOFTGL_VARY_U             0
#define SOFTGL_VARY_V             1
#define SOFTGL_VARY_NX            2
#define SOFTGL_VARY_NY            3
#define SOFTGL_VARY_NZ            4
#define SOFTGL_VARY_WX            5
#define SOFTGL_VARY_WY            6
#define SOFTGL_VARY_WZ            7

/* Binary mesh container: 'SGLM', little endian. */

#define SOFTGL_MESH_MAGIC         0x4d4c4753u
#define SOFTGL_MESH_VERSION       1

/* Depth buffer is 16-bit; 0 == near plane, SOFTGL_DEPTH_MAX == far plane. */

#define SOFTGL_DEPTH_MAX          65535u

#define SOFTGL_PI                 3.14159265358979323846f
#define SOFTGL_DEG2RAD(d)         ((d) * (SOFTGL_PI / 180.0f))

/* RGB565 helpers. */

#define SOFTGL_RGB565(r, g, b) \
  ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Matrix slots addressed by softgl_set_matrix(). */

enum softgl_matrix_e
{
  SOFTGL_MATRIX_MODEL = 0,
  SOFTGL_MATRIX_VIEW,
  SOFTGL_MATRIX_PROJECTION
};

enum softgl_cull_e
{
  SOFTGL_CULL_NONE = 0,
  SOFTGL_CULL_BACK,
  SOFTGL_CULL_FRONT
};

enum softgl_filter_e
{
  SOFTGL_FILTER_NEAREST = 0,
  SOFTGL_FILTER_BILINEAR
};

enum softgl_shade_e
{
  SOFTGL_SHADE_UNLIT = 0,      /* base colour * texture only            */
  SOFTGL_SHADE_LAMBERT,        /* ambient + diffuse                     */
  SOFTGL_SHADE_PHONG           /* ambient + diffuse + Blinn-Phong spec  */
};

/* Vector / matrix types.  Matrices are column-major (OpenGL layout), i.e.
 * m[col * 4 + row], so that a transform is applied as v' = M * v.
 */

struct softgl_vec2_s
{
  float x;
  float y;
};

struct softgl_vec3_s
{
  float x;
  float y;
  float z;
};

struct softgl_vec4_s
{
  float x;
  float y;
  float z;
  float w;
};

struct softgl_mat4_s
{
  float m[16];
};

struct softgl_quat_s
{
  float x;
  float y;
  float z;
  float w;
};

/* Interleaved vertex, as stored in a mesh and in the binary container.
 * All members are 32-bit floats so the layout has no implicit padding and
 * maps 1:1 onto the on-disk representation.
 */

struct softgl_vertex_s
{
  float pos[3];
  float nrm[3];
  float uv[2];
};

/* RGB565 texture.  Dimensions do not have to be powers of two; UVs are
 * wrapped by repeat.
 */

struct softgl_texture_s
{
  const uint16_t *pixels;
  uint16_t        width;
  uint16_t        height;
  bool            owned;       /* pixels were malloc'ed by softgl */
};

/* An indexed triangle mesh. */

struct softgl_mesh_s
{
  const struct softgl_vertex_s *vertices;
  const uint16_t               *indices;
  uint32_t                      nvertices;
  uint32_t                      nindices;   /* multiple of 3 */
  const struct softgl_texture_s *texture;   /* NULL -> untextured */
  float                         basecolor[3];
  bool                          owned;      /* arrays were malloc'ed */
};

/* On-disk header of a .mesh file produced by tools/obj_to_mesh.py.
 * 40 bytes, naturally aligned, little endian.
 */

struct softgl_mesh_header_s
{
  uint32_t magic;              /* SOFTGL_MESH_MAGIC   */
  uint32_t version;            /* SOFTGL_MESH_VERSION */
  uint32_t nvertices;
  uint32_t nindices;
  float    bbox_min[3];
  float    bbox_max[3];
};

/* Scene lighting.  The direction points *from* the surface *towards* the
 * light and is expected to be normalised in world space.
 */

struct softgl_light_s
{
  struct softgl_vec3_s direction;
  struct softgl_vec3_s color;
  struct softgl_vec3_s ambient;
  float                specular;    /* specular strength, 0..1 */
  float                shininess;   /* Blinn-Phong exponent    */
};

/* Internal per-vertex output of the vertex stage. */

struct softgl_vsout_s
{
  struct softgl_vec4_s clip;
  struct softgl_vec3_s wpos;
  struct softgl_vec3_s wnrm;
  float                u;
  float                v;
};

/* A screen-space triangle ready to be scan converted.  Varyings are already
 * divided by w so that they can be interpolated linearly in screen space.
 */

struct softgl_rtri_s
{
  float x[3];
  float y[3];
  float z[3];                                  /* window depth, 0..1     */
  float iw[3];                                 /* 1 / clip.w             */
  float var[3][SOFTGL_NUM_VARYINGS];           /* varying * iw           */
  float bias[3];                               /* top-left fill rule     */
  float inv_area;
  int   minx;
  int   maxx;
  int   miny;
  int   maxy;
  const struct softgl_texture_s *tex;
  float base[3];
};

struct softgl_context_s;

/* One rasteriser worker. */

struct softgl_worker_s
{
  pthread_t                tid;
  struct softgl_context_s *ctx;
  int                      band;
  uint32_t                 seq;
  bool                     started;
};

struct softgl_context_s
{
  int       width;
  int       height;
  int       stride;             /* colour buffer stride in pixels */
  uint16_t *color;
  uint16_t *depth;
  bool      own_color;
  bool      own_depth;

  /* Transforms. */

  struct softgl_mat4_s model;
  struct softgl_mat4_s view;
  struct softgl_mat4_s proj;
  struct softgl_mat4_s mvp;         /* proj * view * model  */
  struct softgl_mat4_s normalmat;   /* inverse-transpose of model */
  struct softgl_vec3_s eye;         /* camera position, world space */
  bool                 dirty;

  /* State. */

  enum softgl_cull_e   cull;
  enum softgl_filter_e filter;
  enum softgl_shade_e  shade;
  bool                 depth_test;
  bool                 depth_write;
  struct softgl_light_s light;
  const struct softgl_texture_s *texture;

  /* Vertex stage scratch. */

  struct softgl_vsout_s *vsbuf;
  uint32_t               vscap;

  /* Triangle bin (shared by every band). */

  struct softgl_rtri_s  *tris;
  uint32_t               ntris;
  uint32_t               tricap;

  /* Statistics for the current frame. */

  uint32_t stat_tris_in;
  uint32_t stat_tris_drawn;
  uint32_t stat_pixels;

  /* Worker pool.  nbands == nworkers + 1 (the caller rasterises band 0). */

  int                    nbands;
  int                    nworkers;
  struct softgl_worker_s workers[SOFTGL_MAX_THREADS - 1];
  pthread_mutex_t        lock;
  pthread_cond_t         start_cv;
  pthread_cond_t         done_cv;
  uint32_t               job_seq;
  int                    pending;
  bool                   shutdown;

  /* Optional /dev/fbN presentation target. */

  int       fbfd;
  uint16_t *fbmem;
  size_t    fblen;
  int       fbstride;           /* in pixels */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/* Vector maths (softgl_math.c)
 * -------------------------------------------------------------------------
 */

struct softgl_vec3_s softgl_vec3(float x, float y, float z);
struct softgl_vec3_s softgl_vec3_add(struct softgl_vec3_s a,
                                     struct softgl_vec3_s b);
struct softgl_vec3_s softgl_vec3_sub(struct softgl_vec3_s a,
                                     struct softgl_vec3_s b);
struct softgl_vec3_s softgl_vec3_scale(struct softgl_vec3_s a, float s);
struct softgl_vec3_s softgl_vec3_cross(struct softgl_vec3_s a,
                                       struct softgl_vec3_s b);
float                softgl_vec3_dot(struct softgl_vec3_s a,
                                     struct softgl_vec3_s b);
float                softgl_vec3_length(struct softgl_vec3_s a);
struct softgl_vec3_s softgl_vec3_normalize(struct softgl_vec3_s a);

struct softgl_vec2_s softgl_vec2(float x, float y);
struct softgl_vec4_s softgl_vec4(float x, float y, float z, float w);

void softgl_mat4_identity(struct softgl_mat4_s *out);
void softgl_mat4_mul(struct softgl_mat4_s *out,
                     const struct softgl_mat4_s *a,
                     const struct softgl_mat4_s *b);
struct softgl_vec4_s softgl_mat4_mul_vec4(const struct softgl_mat4_s *m,
                                          struct softgl_vec4_s v);
struct softgl_vec3_s softgl_mat4_mul_point(const struct softgl_mat4_s *m,
                                           struct softgl_vec3_s v);
struct softgl_vec3_s softgl_mat4_mul_dir(const struct softgl_mat4_s *m,
                                         struct softgl_vec3_s v);
void softgl_mat4_transpose(struct softgl_mat4_s *out,
                           const struct softgl_mat4_s *a);
bool softgl_mat4_inverse(struct softgl_mat4_s *out,
                         const struct softgl_mat4_s *a);
void softgl_mat4_translate(struct softgl_mat4_s *out, float x, float y,
                           float z);
void softgl_mat4_scale(struct softgl_mat4_s *out, float x, float y, float z);
void softgl_mat4_rotate(struct softgl_mat4_s *out, struct softgl_vec3_s axis,
                        float radians);
void softgl_mat4_perspective(struct softgl_mat4_s *out, float fovy_radians,
                             float aspect, float znear, float zfar);
void softgl_mat4_ortho(struct softgl_mat4_s *out, float left, float right,
                       float bottom, float top, float znear, float zfar);
void softgl_mat4_lookat(struct softgl_mat4_s *out, struct softgl_vec3_s eye,
                        struct softgl_vec3_s center,
                        struct softgl_vec3_s up);

struct softgl_quat_s softgl_quat_identity(void);
struct softgl_quat_s softgl_quat_axis_angle(struct softgl_vec3_s axis,
                                            float radians);
struct softgl_quat_s softgl_quat_mul(struct softgl_quat_s a,
                                     struct softgl_quat_s b);
struct softgl_quat_s softgl_quat_normalize(struct softgl_quat_s q);
struct softgl_quat_s softgl_quat_slerp(struct softgl_quat_s a,
                                       struct softgl_quat_s b, float t);
void softgl_quat_to_mat4(struct softgl_mat4_s *out, struct softgl_quat_s q);

/* Context and resources (softgl.c)
 * -------------------------------------------------------------------------
 */

struct softgl_context_s *softgl_create_context(int width, int height,
                                               uint16_t *framebuffer);
void softgl_destroy_context(struct softgl_context_s *ctx);

int  softgl_set_threads(struct softgl_context_s *ctx, int nthreads);
void softgl_clear(struct softgl_context_s *ctx, uint16_t color, bool depth);
void softgl_set_matrix(struct softgl_context_s *ctx,
                       enum softgl_matrix_e which,
                       const struct softgl_mat4_s *m);
void softgl_bind_texture(struct softgl_context_s *ctx,
                         const struct softgl_texture_s *tex);
void softgl_set_light(struct softgl_context_s *ctx,
                      const struct softgl_light_s *light);
void softgl_set_cull(struct softgl_context_s *ctx, enum softgl_cull_e cull);
void softgl_set_filter(struct softgl_context_s *ctx,
                       enum softgl_filter_e filter);
void softgl_set_shading(struct softgl_context_s *ctx,
                        enum softgl_shade_e shade);

int  softgl_bind_fbdev(struct softgl_context_s *ctx, const char *devpath);
int  softgl_present(struct softgl_context_s *ctx);
int  softgl_write_ppm(struct softgl_context_s *ctx, const char *path);

/* Internal, shared between softgl.c and softgl_raster.c. */

void softgl_sync_state(struct softgl_context_s *ctx);
void softgl_dispatch_bands(struct softgl_context_s *ctx);

/* Mesh and texture helpers. */

int  softgl_mesh_load_memory(struct softgl_mesh_s *mesh, const void *data,
                             size_t len);
int  softgl_mesh_load(struct softgl_mesh_s *mesh, const char *path);
int  softgl_mesh_load_obj(struct softgl_mesh_s *mesh, const char *path);
void softgl_mesh_free(struct softgl_mesh_s *mesh);
void softgl_texture_free(struct softgl_texture_s *tex);

/* Rasteriser (softgl_raster.c)
 * -------------------------------------------------------------------------
 */

int  softgl_draw_mesh(struct softgl_context_s *ctx,
                      const struct softgl_mesh_s *mesh);
void softgl_raster_band(struct softgl_context_s *ctx, int band);
void softgl_fill16(uint16_t *dst, uint16_t value, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* __APP_SOFTGL_SOFTGL_H */
