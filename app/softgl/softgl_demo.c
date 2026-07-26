/****************************************************************************
 * app/softgl/softgl_demo.c
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
 * SoftGL demonstration / benchmark for the KICKPI-K7 round panels.
 *
 * Renders a rotating low-poly scene at 360x360 (the Nyabula display size)
 * with texturing, per-pixel Blinn-Phong lighting and a Z-buffer, entirely
 * on the Cortex-A cores.  Output goes to /dev/fb0 when a 16bpp framebuffer
 * is available, otherwise to a PPM file or nowhere at all (pure benchmark).
 *
 * Usage: softgl [options]
 *   -s <cube|cat>   scene to render (default cat)
 *   -m <file>       load a .mesh file instead of a built-in scene
 *   -r <pixels>     square render resolution (default 360)
 *   -n <frames>     number of frames to render (default 120, 0 = forever)
 *   -t <threads>    rasteriser threads including the caller (default: SMP
 *                   CPU count)
 *   -d <device>     framebuffer device (default /dev/fb0)
 *   -o <file.ppm>   write the last frame to a PPM file
 *   -f <0|1>        texture filter, 0 nearest 1 bilinear (default 1)
 *   -l <0|1|2>      shading, 0 unlit 1 lambert 2 blinn-phong (default 2)
 *   -x              skip the framebuffer entirely (benchmark only)
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "softgl.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SOFTGL_DEMO_RES_DEFAULT   360
#define SOFTGL_DEMO_FRAMES        120
#define SOFTGL_DEMO_TEXSIZE       64
#define SOFTGL_DEMO_FPS_WINDOW    30

/* Camera. */

#define SOFTGL_DEMO_FOV_DEG       42.0f
#define SOFTGL_DEMO_ZNEAR         0.2f
#define SOFTGL_DEMO_ZFAR          40.0f

/* Background: a dark warm grey that suits the round panels. */

#define SOFTGL_DEMO_CLEAR         SOFTGL_RGB565(18, 16, 24)

#define SOFTGL_DEMO_NSEC_PER_SEC  1000000000ll

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Incremental mesh builder used to assemble the procedural scenes. */

struct softgl_demo_builder_s
{
  struct softgl_vertex_s *verts;
  uint16_t               *idx;
  uint32_t                nverts;
  uint32_t                nidx;
  uint32_t                vcap;
  uint32_t                icap;
  bool                    failed;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int64_t softgl_demo_now_ns(void);
static void softgl_demo_builder_init(struct softgl_demo_builder_s *bld);
static bool softgl_demo_builder_grow(struct softgl_demo_builder_s *bld,
                                     uint32_t addverts, uint32_t addidx);
static void softgl_demo_add_quad(struct softgl_demo_builder_s *bld,
                                 struct softgl_vec3_s p0,
                                 struct softgl_vec3_s p1,
                                 struct softgl_vec3_s p2,
                                 struct softgl_vec3_s p3, float uscale,
                                 float vscale);
static void softgl_demo_add_box(struct softgl_demo_builder_s *bld,
                                struct softgl_vec3_s center,
                                struct softgl_vec3_s halfsize,
                                float uvscale);
static void softgl_demo_add_pyramid(struct softgl_demo_builder_s *bld,
                                    struct softgl_vec3_s base_center,
                                    struct softgl_vec3_s halfsize,
                                    float height);
static bool softgl_demo_builder_finish(struct softgl_demo_builder_s *bld,
                                       struct softgl_mesh_s *mesh);
static int softgl_demo_make_cube(struct softgl_mesh_s *mesh);
static int softgl_demo_make_cat(struct softgl_mesh_s *mesh);
static int softgl_demo_make_texture(struct softgl_texture_s *tex,
                                    bool checker);
static void softgl_demo_usage(const char *progname);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: softgl_demo_now_ns
 ****************************************************************************/

static int64_t softgl_demo_now_ns(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * SOFTGL_DEMO_NSEC_PER_SEC + ts.tv_nsec;
}

/****************************************************************************
 * Name: softgl_demo_builder_init
 ****************************************************************************/

static void softgl_demo_builder_init(struct softgl_demo_builder_s *bld)
{
  memset(bld, 0, sizeof(*bld));
}

/****************************************************************************
 * Name: softgl_demo_builder_grow
 ****************************************************************************/

static bool softgl_demo_builder_grow(struct softgl_demo_builder_s *bld,
                                     uint32_t addverts, uint32_t addidx)
{
  if (bld->failed)
    {
      return false;
    }

  if (bld->nverts + addverts > bld->vcap)
    {
      uint32_t cap = bld->vcap != 0 ? bld->vcap * 2 : 64;
      struct softgl_vertex_s *p;

      while (cap < bld->nverts + addverts)
        {
          cap *= 2;
        }

      p = (struct softgl_vertex_s *)realloc(bld->verts,
                                            (size_t)cap * sizeof(*p));
      if (p == NULL)
        {
          bld->failed = true;
          return false;
        }

      bld->verts = p;
      bld->vcap  = cap;
    }

  if (bld->nidx + addidx > bld->icap)
    {
      uint32_t cap = bld->icap != 0 ? bld->icap * 2 : 128;
      uint16_t *p;

      while (cap < bld->nidx + addidx)
        {
          cap *= 2;
        }

      p = (uint16_t *)realloc(bld->idx, (size_t)cap * sizeof(*p));
      if (p == NULL)
        {
          bld->failed = true;
          return false;
        }

      bld->idx  = p;
      bld->icap = cap;
    }

  return true;
}

/****************************************************************************
 * Name: softgl_demo_add_quad
 *
 * Description:
 *   Append one flat-shaded quad given its four corners in counter-clockwise
 *   order as seen from the outside.  The face normal is derived from the
 *   geometry, so callers never have to keep normals in sync with scaling.
 *
 ****************************************************************************/

static void softgl_demo_add_quad(struct softgl_demo_builder_s *bld,
                                 struct softgl_vec3_s p0,
                                 struct softgl_vec3_s p1,
                                 struct softgl_vec3_s p2,
                                 struct softgl_vec3_s p3, float uscale,
                                 float vscale)
{
  struct softgl_vec3_s n;
  struct softgl_vec3_s corner[4];
  const float uv[4][2] =
  {
    {
      0.0f, 0.0f
    },
    {
      1.0f, 0.0f
    },
    {
      1.0f, 1.0f
    },
    {
      0.0f, 1.0f
    }
  };

  uint16_t base;
  int i;

  if (!softgl_demo_builder_grow(bld, 4, 6))
    {
      return;
    }

  n = softgl_vec3_normalize(softgl_vec3_cross(softgl_vec3_sub(p1, p0),
                                              softgl_vec3_sub(p3, p0)));

  corner[0] = p0;
  corner[1] = p1;
  corner[2] = p2;
  corner[3] = p3;

  base = (uint16_t)bld->nverts;

  for (i = 0; i < 4; i++)
    {
      struct softgl_vertex_s *v = &bld->verts[bld->nverts++];

      v->pos[0] = corner[i].x;
      v->pos[1] = corner[i].y;
      v->pos[2] = corner[i].z;
      v->nrm[0] = n.x;
      v->nrm[1] = n.y;
      v->nrm[2] = n.z;
      v->uv[0]  = uv[i][0] * uscale;
      v->uv[1]  = uv[i][1] * vscale;
    }

  bld->idx[bld->nidx++] = base;
  bld->idx[bld->nidx++] = (uint16_t)(base + 1);
  bld->idx[bld->nidx++] = (uint16_t)(base + 2);
  bld->idx[bld->nidx++] = base;
  bld->idx[bld->nidx++] = (uint16_t)(base + 2);
  bld->idx[bld->nidx++] = (uint16_t)(base + 3);
}

/****************************************************************************
 * Name: softgl_demo_add_box
 *
 * Description:
 *   Append an axis-aligned box (12 triangles, hard edges).
 *
 ****************************************************************************/

static void softgl_demo_add_box(struct softgl_demo_builder_s *bld,
                                struct softgl_vec3_s center,
                                struct softgl_vec3_s halfsize,
                                float uvscale)
{
  float x0 = center.x - halfsize.x;
  float x1 = center.x + halfsize.x;
  float y0 = center.y - halfsize.y;
  float y1 = center.y + halfsize.y;
  float z0 = center.z - halfsize.z;
  float z1 = center.z + halfsize.z;

  /* +Z */

  softgl_demo_add_quad(bld, softgl_vec3(x0, y0, z1), softgl_vec3(x1, y0, z1),
                       softgl_vec3(x1, y1, z1), softgl_vec3(x0, y1, z1),
                       uvscale, uvscale);

  /* -Z */

  softgl_demo_add_quad(bld, softgl_vec3(x1, y0, z0), softgl_vec3(x0, y0, z0),
                       softgl_vec3(x0, y1, z0), softgl_vec3(x1, y1, z0),
                       uvscale, uvscale);

  /* +X */

  softgl_demo_add_quad(bld, softgl_vec3(x1, y0, z1), softgl_vec3(x1, y0, z0),
                       softgl_vec3(x1, y1, z0), softgl_vec3(x1, y1, z1),
                       uvscale, uvscale);

  /* -X */

  softgl_demo_add_quad(bld, softgl_vec3(x0, y0, z0), softgl_vec3(x0, y0, z1),
                       softgl_vec3(x0, y1, z1), softgl_vec3(x0, y1, z0),
                       uvscale, uvscale);

  /* +Y */

  softgl_demo_add_quad(bld, softgl_vec3(x0, y1, z1), softgl_vec3(x1, y1, z1),
                       softgl_vec3(x1, y1, z0), softgl_vec3(x0, y1, z0),
                       uvscale, uvscale);

  /* -Y */

  softgl_demo_add_quad(bld, softgl_vec3(x0, y0, z0), softgl_vec3(x1, y0, z0),
                       softgl_vec3(x1, y0, z1), softgl_vec3(x0, y0, z1),
                       uvscale, uvscale);
}

/****************************************************************************
 * Name: softgl_demo_add_pyramid
 *
 * Description:
 *   Append a rectangular pyramid standing on +Y, used for the cat's ears.
 *
 ****************************************************************************/

static void softgl_demo_add_pyramid(struct softgl_demo_builder_s *bld,
                                    struct softgl_vec3_s base_center,
                                    struct softgl_vec3_s halfsize,
                                    float height)
{
  struct softgl_vec3_s apex = softgl_vec3(base_center.x,
                                          base_center.y + height,
                                          base_center.z);
  struct softgl_vec3_s c[4];
  uint16_t base;
  int i;

  c[0] = softgl_vec3(base_center.x - halfsize.x, base_center.y,
                     base_center.z + halfsize.z);
  c[1] = softgl_vec3(base_center.x + halfsize.x, base_center.y,
                     base_center.z + halfsize.z);
  c[2] = softgl_vec3(base_center.x + halfsize.x, base_center.y,
                     base_center.z - halfsize.z);
  c[3] = softgl_vec3(base_center.x - halfsize.x, base_center.y,
                     base_center.z - halfsize.z);

  for (i = 0; i < 4; i++)
    {
      struct softgl_vec3_s a = c[i];
      struct softgl_vec3_s b = c[(i + 1) % 4];
      struct softgl_vec3_s n;
      int k;
      struct softgl_vec3_s tri[3];
      const float triuv[3][2] =
      {
        {
          0.0f, 0.0f
        },
        {
          1.0f, 0.0f
        },
        {
          0.5f, 1.0f
        }
      };

      if (!softgl_demo_builder_grow(bld, 3, 3))
        {
          return;
        }

      tri[0] = a;
      tri[1] = b;
      tri[2] = apex;

      n = softgl_vec3_normalize(softgl_vec3_cross(softgl_vec3_sub(b, a),
                                                  softgl_vec3_sub(apex, a)));

      base = (uint16_t)bld->nverts;

      for (k = 0; k < 3; k++)
        {
          struct softgl_vertex_s *v = &bld->verts[bld->nverts++];

          v->pos[0] = tri[k].x;
          v->pos[1] = tri[k].y;
          v->pos[2] = tri[k].z;
          v->nrm[0] = n.x;
          v->nrm[1] = n.y;
          v->nrm[2] = n.z;
          v->uv[0]  = triuv[k][0];
          v->uv[1]  = triuv[k][1];
        }

      bld->idx[bld->nidx++] = base;
      bld->idx[bld->nidx++] = (uint16_t)(base + 1);
      bld->idx[bld->nidx++] = (uint16_t)(base + 2);
    }

  /* Close the bottom so the ear is watertight for the depth buffer. */

  softgl_demo_add_quad(bld, c[3], c[2], c[1], c[0], 1.0f, 1.0f);
}

/****************************************************************************
 * Name: softgl_demo_builder_finish
 ****************************************************************************/

static bool softgl_demo_builder_finish(struct softgl_demo_builder_s *bld,
                                       struct softgl_mesh_s *mesh)
{
  if (bld->failed || bld->nverts == 0 || bld->nidx == 0)
    {
      free(bld->verts);
      free(bld->idx);
      softgl_demo_builder_init(bld);
      return false;
    }

  memset(mesh, 0, sizeof(*mesh));
  mesh->vertices     = bld->verts;
  mesh->indices      = bld->idx;
  mesh->nvertices    = bld->nverts;
  mesh->nindices     = bld->nidx;
  mesh->owned        = true;
  mesh->basecolor[0] = 1.0f;
  mesh->basecolor[1] = 1.0f;
  mesh->basecolor[2] = 1.0f;

  softgl_demo_builder_init(bld);
  return true;
}

/****************************************************************************
 * Name: softgl_demo_make_cube
 ****************************************************************************/

static int softgl_demo_make_cube(struct softgl_mesh_s *mesh)
{
  struct softgl_demo_builder_s bld;

  softgl_demo_builder_init(&bld);
  softgl_demo_add_box(&bld, softgl_vec3(0.0f, 0.0f, 0.0f),
                      softgl_vec3(1.0f, 1.0f, 1.0f), 1.0f);

  return softgl_demo_builder_finish(&bld, mesh) ? OK : -ENOMEM;
}

/****************************************************************************
 * Name: softgl_demo_make_cat
 *
 * Description:
 *   Build the stylised low-poly Nyabula cat out of boxes and two pyramid
 *   ears: roughly 30 quads / 120 triangles, which is the polygon budget the
 *   "cat house" mode targets on this CPU rasteriser.
 *
 ****************************************************************************/

static int softgl_demo_make_cat(struct softgl_mesh_s *mesh)
{
  struct softgl_demo_builder_s bld;

  softgl_demo_builder_init(&bld);

  /* Body. */

  softgl_demo_add_box(&bld, softgl_vec3(0.0f, 0.0f, 0.0f),
                      softgl_vec3(0.75f, 0.45f, 0.45f), 2.0f);

  /* Head. */

  softgl_demo_add_box(&bld, softgl_vec3(0.95f, 0.55f, 0.0f),
                      softgl_vec3(0.42f, 0.38f, 0.38f), 1.0f);

  /* Snout. */

  softgl_demo_add_box(&bld, softgl_vec3(1.42f, 0.42f, 0.0f),
                      softgl_vec3(0.14f, 0.14f, 0.18f), 1.0f);

  /* Ears. */

  softgl_demo_add_pyramid(&bld, softgl_vec3(0.85f, 0.93f, 0.22f),
                          softgl_vec3(0.14f, 0.0f, 0.10f), 0.30f);
  softgl_demo_add_pyramid(&bld, softgl_vec3(0.85f, 0.93f, -0.22f),
                          softgl_vec3(0.14f, 0.0f, 0.10f), 0.30f);

  /* Legs. */

  softgl_demo_add_box(&bld, softgl_vec3(0.52f, -0.62f, 0.30f),
                      softgl_vec3(0.13f, 0.22f, 0.13f), 1.0f);
  softgl_demo_add_box(&bld, softgl_vec3(0.52f, -0.62f, -0.30f),
                      softgl_vec3(0.13f, 0.22f, 0.13f), 1.0f);
  softgl_demo_add_box(&bld, softgl_vec3(-0.52f, -0.62f, 0.30f),
                      softgl_vec3(0.13f, 0.22f, 0.13f), 1.0f);
  softgl_demo_add_box(&bld, softgl_vec3(-0.52f, -0.62f, -0.30f),
                      softgl_vec3(0.13f, 0.22f, 0.13f), 1.0f);

  /* Tail: three shrinking segments curving up behind the body. */

  softgl_demo_add_box(&bld, softgl_vec3(-0.86f, 0.10f, 0.0f),
                      softgl_vec3(0.14f, 0.10f, 0.10f), 1.0f);
  softgl_demo_add_box(&bld, softgl_vec3(-1.04f, 0.36f, 0.0f),
                      softgl_vec3(0.09f, 0.20f, 0.09f), 1.0f);
  softgl_demo_add_box(&bld, softgl_vec3(-0.94f, 0.62f, 0.0f),
                      softgl_vec3(0.16f, 0.08f, 0.08f), 1.0f);

  return softgl_demo_builder_finish(&bld, mesh) ? OK : -ENOMEM;
}

/****************************************************************************
 * Name: softgl_demo_make_texture
 *
 * Description:
 *   Generate a procedural RGB565 texture: either a checkerboard (useful for
 *   eyeballing perspective correctness and filtering) or a warm tabby fur
 *   pattern for the cat.
 *
 ****************************************************************************/

static int softgl_demo_make_texture(struct softgl_texture_s *tex,
                                    bool checker)
{
  const int size = SOFTGL_DEMO_TEXSIZE;
  uint16_t *px;
  int x;
  int y;

  px = (uint16_t *)malloc((size_t)size * size * sizeof(uint16_t));
  if (px == NULL)
    {
      return -ENOMEM;
    }

  for (y = 0; y < size; y++)
    {
      for (x = 0; x < size; x++)
        {
          int r;
          int g;
          int b;

          if (checker)
            {
              bool odd = (((x >> 3) + (y >> 3)) & 1) != 0;

              r = odd ? 235 : 40;
              g = odd ? 235 : 60;
              b = odd ? 245 : 90;
            }
          else
            {
              /* Tabby: warm base with darker vertical stripes and a little
               * per-texel noise so the surface is not flat.
               */

              int stripe = (int)(24.0f *
                            sinf((float)x * 0.55f) *
                            sinf((float)y * 0.13f));
              int noise = ((x * 37 + y * 17) & 7) - 4;

              r = 214 - stripe + noise;
              g = 156 - stripe + noise;
              b = 104 - stripe + noise;
            }

          r = r < 0 ? 0 : (r > 255 ? 255 : r);
          g = g < 0 ? 0 : (g > 255 ? 255 : g);
          b = b < 0 ? 0 : (b > 255 ? 255 : b);

          px[y * size + x] = SOFTGL_RGB565(r, g, b);
        }
    }

  memset(tex, 0, sizeof(*tex));
  tex->pixels = px;
  tex->width  = (uint16_t)size;
  tex->height = (uint16_t)size;
  tex->owned  = true;
  return OK;
}

/****************************************************************************
 * Name: softgl_demo_usage
 ****************************************************************************/

static void softgl_demo_usage(const char *progname)
{
  printf("Usage: %s [options]\n"
         "  -s <cube|cat>  built-in scene (default cat)\n"
         "  -m <file>      load a .mesh file instead\n"
         "  -r <pixels>    square resolution (default %d)\n"
         "  -n <frames>    frames to render, 0 = forever (default %d)\n"
         "  -t <threads>   rasteriser threads incl. caller\n"
         "  -d <device>    framebuffer device (default /dev/fb0)\n"
         "  -o <file.ppm>  dump the last frame\n"
         "  -f <0|1>       texture filter: nearest / bilinear\n"
         "  -l <0|1|2>     shading: unlit / lambert / blinn-phong\n"
         "  -x             benchmark only, do not touch the framebuffer\n",
         progname, SOFTGL_DEMO_RES_DEFAULT, SOFTGL_DEMO_FRAMES);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct softgl_context_s *ctx;
  struct softgl_mesh_s mesh;
  struct softgl_texture_s tex;
  struct softgl_mat4_s proj;
  struct softgl_mat4_s view;
  struct softgl_mat4_s model;
  struct softgl_mat4_s spin;
  struct softgl_mat4_s tilt;
  struct softgl_light_s light;
  const char *scene = "cat";
  const char *meshfile = NULL;
  const char *fbdev = "/dev/fb0";
  const char *ppmout = NULL;
  int res = SOFTGL_DEMO_RES_DEFAULT;
  int frames = SOFTGL_DEMO_FRAMES;
  int threads = 0;
  int filter = SOFTGL_FILTER_BILINEAR;
  int shading = SOFTGL_SHADE_PHONG;
  bool nofb = false;
  bool have_tex = false;
  bool have_mesh = false;
  int64_t t_window;
  int64_t t_total;
  int64_t worst = 0;
  double elapsed;
  int frame;
  int ret;
  int i;

  for (i = 1; i < argc; i++)
    {
      const char *a = argv[i];

      if (strcmp(a, "-x") == 0)
        {
          nofb = true;
        }
      else if (strcmp(a, "-h") == 0)
        {
          softgl_demo_usage(argv[0]);
          return EXIT_SUCCESS;
        }
      else if (i + 1 >= argc)
        {
          softgl_demo_usage(argv[0]);
          return EXIT_FAILURE;
        }
      else if (strcmp(a, "-s") == 0)
        {
          scene = argv[++i];
        }
      else if (strcmp(a, "-m") == 0)
        {
          meshfile = argv[++i];
        }
      else if (strcmp(a, "-r") == 0)
        {
          res = atoi(argv[++i]);
        }
      else if (strcmp(a, "-n") == 0)
        {
          frames = atoi(argv[++i]);
        }
      else if (strcmp(a, "-t") == 0)
        {
          threads = atoi(argv[++i]);
        }
      else if (strcmp(a, "-d") == 0)
        {
          fbdev = argv[++i];
        }
      else if (strcmp(a, "-o") == 0)
        {
          ppmout = argv[++i];
        }
      else if (strcmp(a, "-f") == 0)
        {
          filter = atoi(argv[++i]);
        }
      else if (strcmp(a, "-l") == 0)
        {
          shading = atoi(argv[++i]);
        }
      else
        {
          softgl_demo_usage(argv[0]);
          return EXIT_FAILURE;
        }
    }

  if (res < 16 || res > 2048)
    {
      fprintf(stderr, "softgl: bad resolution %d\n", res);
      return EXIT_FAILURE;
    }

  /* ---- Scene ---------------------------------------------------------- */

  if (meshfile != NULL)
    {
      size_t len = strlen(meshfile);

      if (len > 4 && strcmp(meshfile + len - 4, ".obj") == 0)
        {
          ret = softgl_mesh_load_obj(&mesh, meshfile);
        }
      else
        {
          ret = softgl_mesh_load(&mesh, meshfile);
        }

      if (ret < 0)
        {
          fprintf(stderr, "softgl: cannot load %s: %d\n", meshfile, ret);
          return EXIT_FAILURE;
        }
    }
  else if (strcmp(scene, "cube") == 0)
    {
      ret = softgl_demo_make_cube(&mesh);
    }
  else
    {
      ret = softgl_demo_make_cat(&mesh);
    }

  if (ret < 0)
    {
      fprintf(stderr, "softgl: cannot build scene: %d\n", ret);
      return EXIT_FAILURE;
    }

  have_mesh = true;

  ret = softgl_demo_make_texture(&tex, strcmp(scene, "cube") == 0);
  if (ret < 0)
    {
      fprintf(stderr, "softgl: cannot build texture: %d\n", ret);
      goto errout;
    }

  have_tex = true;

  /* ---- Context -------------------------------------------------------- */

  ctx = softgl_create_context(res, res, NULL);
  if (ctx == NULL)
    {
      fprintf(stderr, "softgl: out of memory for %dx%d buffers\n", res, res);
      ret = -ENOMEM;
      goto errout;
    }

  if (threads > 0)
    {
      softgl_set_threads(ctx, threads);
    }

  if (!nofb)
    {
      ret = softgl_bind_fbdev(ctx, fbdev);
      if (ret < 0)
        {
          printf("softgl: %s unavailable (%d), running headless\n",
                 fbdev, ret);
        }
    }

  softgl_bind_texture(ctx, &tex);
  softgl_set_filter(ctx, (enum softgl_filter_e)filter);
  softgl_set_shading(ctx, (enum softgl_shade_e)shading);
  softgl_set_cull(ctx, SOFTGL_CULL_BACK);

  light.direction = softgl_vec3(-0.45f, 0.75f, 0.5f);
  light.color     = softgl_vec3(0.95f, 0.90f, 0.82f);
  light.ambient   = softgl_vec3(0.22f, 0.22f, 0.30f);
  light.specular  = 0.40f;
  light.shininess = 32.0f;
  softgl_set_light(ctx, &light);

  softgl_mat4_perspective(&proj, SOFTGL_DEG2RAD(SOFTGL_DEMO_FOV_DEG),
                          1.0f, SOFTGL_DEMO_ZNEAR, SOFTGL_DEMO_ZFAR);
  softgl_set_matrix(ctx, SOFTGL_MATRIX_PROJECTION, &proj);

  softgl_mat4_lookat(&view, softgl_vec3(0.0f, 1.6f, 5.2f),
                     softgl_vec3(0.0f, 0.05f, 0.0f),
                     softgl_vec3(0.0f, 1.0f, 0.0f));
  softgl_set_matrix(ctx, SOFTGL_MATRIX_VIEW, &view);

  printf("softgl: %dx%d RGB565, %u tris, %d raster band(s)\n",
         res, res, (unsigned)(mesh.nindices / 3), ctx->nbands);

  /* ---- Main loop ------------------------------------------------------ */

  t_total  = softgl_demo_now_ns();
  t_window = t_total;

  for (frame = 0; frames == 0 || frame < frames; frame++)
    {
      float angle = (float)frame * 0.035f;
      int64_t t0 = softgl_demo_now_ns();
      int64_t dt;

      softgl_mat4_rotate(&spin, softgl_vec3(0.0f, 1.0f, 0.0f), angle);
      softgl_mat4_rotate(&tilt, softgl_vec3(1.0f, 0.0f, 0.0f),
                         0.18f * sinf(angle * 1.7f));
      softgl_mat4_mul(&model, &spin, &tilt);
      softgl_set_matrix(ctx, SOFTGL_MATRIX_MODEL, &model);

      softgl_clear(ctx, SOFTGL_DEMO_CLEAR, true);

      ret = softgl_draw_mesh(ctx, &mesh);
      if (ret < 0)
        {
          fprintf(stderr, "softgl: draw failed: %d\n", ret);
          break;
        }

      softgl_present(ctx);

      dt = softgl_demo_now_ns() - t0;
      if (dt > worst)
        {
          worst = dt;
        }

      if (((frame + 1) % SOFTGL_DEMO_FPS_WINDOW) == 0)
        {
          int64_t now = softgl_demo_now_ns();
          double secs = (double)(now - t_window) /
                        (double)SOFTGL_DEMO_NSEC_PER_SEC;

          printf("softgl: frame %d  %.1f fps  (%.2f ms/frame, "
                 "worst %.2f ms, %u tris drawn)\n",
                 frame + 1,
                 secs > 0.0 ? (double)SOFTGL_DEMO_FPS_WINDOW / secs : 0.0,
                 secs * 1000.0 / (double)SOFTGL_DEMO_FPS_WINDOW,
                 (double)worst / 1.0e6,
                 (unsigned)ctx->stat_tris_drawn);

          fflush(stdout);
          t_window = now;
          worst    = 0;
          ctx->stat_tris_drawn = 0;
        }
    }

  if (ppmout != NULL)
    {
      ret = softgl_write_ppm(ctx, ppmout);
      if (ret < 0)
        {
          fprintf(stderr, "softgl: cannot write %s: %d\n", ppmout, ret);
        }
      else
        {
          printf("softgl: wrote %s\n", ppmout);
        }
    }

  elapsed = (double)(softgl_demo_now_ns() - t_total) /
            (double)SOFTGL_DEMO_NSEC_PER_SEC;

  printf("softgl: %d frames in %.2f s -> %.1f fps average\n",
         frame, elapsed, elapsed > 0.0 ? (double)frame / elapsed : 0.0);

  softgl_destroy_context(ctx);
  softgl_texture_free(&tex);
  softgl_mesh_free(&mesh);
  return EXIT_SUCCESS;

errout:
  if (have_tex)
    {
      softgl_texture_free(&tex);
    }

  if (have_mesh)
    {
      softgl_mesh_free(&mesh);
    }

  return EXIT_FAILURE;
}
