/****************************************************************************
 * app/nyabula/src/nyabula_eye_renderer_lvgl.c
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

#include <nuttx/config.h>

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nyabula_eye_internal.h"
#include "generated/nyabula_eye_icons.h"
#include "generated/fonts/nyabula_eye_fonts.h"

#define W           720
#define H           360
#define R           178.0f
#define GAP         360.0f
#define CY          180.0f
#define PI          3.14159265358979323846f
#define FIBERS      48
#define ZCOUNT      16
#define ICON_CACHE_COUNT 40
#define FONT_CACHE_COUNT 32
#define LID_SAMPLES 400
#define LID_OFFSET  200

struct transform_s
{
  float cx;
  float cy;
  float sy;
  float sn;
  float cs;
};

struct eye_s
{
  struct transform_s t;
  const struct nyabula_eye_frame_s *f;
  float ir;
  float gx;
  float gy;
  float lt;
  float lb;
  float slant;
  float curve;
  float top_y[LID_SAMPLES];
  float bottom_y[LID_SAMPLES];
  int id;
};

struct z_s
{
  float ox;
  float y;
  float vy;
  float sway;
  float phase;
  float size;
  float life;
  float rotation;
  int eye;
  bool active;
};

struct icon_cache_s
{
  const struct nyabula_eye_icon_s *asset;
  uint8_t *coverage;
  uint32_t last_used;
  int size_key;
  int left;
  int top;
  int width;
  int height;
};

enum font_family_e
{
  FONT_FAMILY_TITLE = 0,
  FONT_FAMILY_BODY,
  FONT_FAMILY_ENGLISH
};

struct font_cache_s
{
  lv_font_t *font;
  uint16_t size;
  uint8_t family;
};

struct nyabula_eye_renderer_s
{
  lv_obj_t *canvas;
  lv_draw_buf_t *buffer[2];
  lv_draw_buf_t *draw;
  lv_draw_buf_t *glyph;
  struct z_s z[ZCOUNT];
  struct icon_cache_s icon_cache[ICON_CACHE_COUNT];
  uint32_t icon_cache_clock;
#if defined(CONFIG_CONTEST2026_062_NYABULA_DYNAMIC_FONTS) && LV_USE_FREETYPE
  struct font_cache_s font_cache[FONT_CACHE_COUNT];
#endif
  uint32_t random;
  uint32_t perf_start;
  uint32_t render_total;
  uint32_t frames;
  uint8_t index;
  float znext;
  float last_time;
  const struct eye_s *clip_eye;
  bool clip_lids;
#if defined(CONFIG_CONTEST2026_062_NYABULA_DYNAMIC_FONTS) && LV_USE_FREETYPE
  lv_font_t *font_title_20;
  lv_font_t *font_title_28;
  lv_font_t *font_title_42;
  lv_font_t *font_body_14;
  lv_font_t *font_body_18;
  lv_font_t *font_english_18;
  lv_font_t *font_english_42;
  lv_font_t *font_english_72;
  lv_font_t *font_english_96;
  lv_font_t *font_english_119;
#endif
};

static bool in_lids(const struct eye_s *e, float sx, float sy);

#if defined(CONFIG_CONTEST2026_062_NYABULA_DYNAMIC_FONTS) && LV_USE_FREETYPE
static lv_font_t *renderer_create_font(const char *filename, uint32_t size);
#endif

static const lv_font_t *renderer_font(struct nyabula_eye_renderer_s *r,
                                      const lv_font_t *fallback)
{
#if defined(CONFIG_CONTEST2026_062_NYABULA_DYNAMIC_FONTS) && LV_USE_FREETYPE
#define DYNAMIC_FONT(name) \
  if (fallback == &nyabula_font_##name && r->font_##name != NULL) \
    { \
      return r->font_##name; \
    }

  DYNAMIC_FONT(title_20);
  DYNAMIC_FONT(title_28);
  DYNAMIC_FONT(title_42);
  DYNAMIC_FONT(body_14);
  DYNAMIC_FONT(body_18);
  DYNAMIC_FONT(english_18);
  DYNAMIC_FONT(english_42);
  DYNAMIC_FONT(english_72);
  DYNAMIC_FONT(english_96);
  DYNAMIC_FONT(english_119);
#undef DYNAMIC_FONT
#else
  (void)r;
#endif
  return fallback;
}

static const lv_font_t *scene_font(struct nyabula_eye_renderer_s *r,
                                   enum font_family_e family, uint16_t size,
                                   const lv_font_t *fallback)
{
#if defined(CONFIG_CONTEST2026_062_NYABULA_DYNAMIC_FONTS) && LV_USE_FREETYPE
  const char *filename;
  int index;

  for (index = 0; index < FONT_CACHE_COUNT; index++)
    {
      if (r->font_cache[index].font != NULL &&
          r->font_cache[index].family == family &&
          r->font_cache[index].size == size)
        {
          return r->font_cache[index].font;
        }
    }

  filename = family == FONT_FAMILY_TITLE
                 ? "AlimamaShuHeiTi.ttf"
                 : family == FONT_FAMILY_BODY ? "MiSans-Semibold.ttf"
                                              : "Tinos-Bold.ttf";
  for (index = 0; index < FONT_CACHE_COUNT; index++)
    {
      if (r->font_cache[index].font == NULL)
        {
          lv_font_t *font = renderer_create_font(filename, size);
          if (font != NULL)
            {
              r->font_cache[index].font = font;
              r->font_cache[index].family = family;
              r->font_cache[index].size = size;
              return font;
            }

          break;
        }
    }
#else
  (void)r;
  (void)family;
  (void)size;
#endif
  return renderer_font(r, fallback);
}

static uint32_t utf8_next(const char *text, uint32_t *offset)
{
  const uint8_t *bytes = (const uint8_t *)text;
  uint32_t first = bytes[(*offset)++];

  if (first < 0x80)
    {
      return first;
    }

  if ((first & 0xe0) == 0xc0)
    {
      uint32_t value = (first & 0x1f) << 6;
      return value | (bytes[(*offset)++] & 0x3f);
    }

  if ((first & 0xf0) == 0xe0)
    {
      uint32_t value = (first & 0x0f) << 12;
      value |= (bytes[(*offset)++] & 0x3f) << 6;
      return value | (bytes[(*offset)++] & 0x3f);
    }

  if ((first & 0xf8) == 0xf0)
    {
      uint32_t value = (first & 0x07) << 18;
      value |= (bytes[(*offset)++] & 0x3f) << 12;
      value |= (bytes[(*offset)++] & 0x3f) << 6;
      return value | (bytes[(*offset)++] & 0x3f);
    }

  return 0xfffd;
}

static float clampf(float value, float low, float high)
{
  return value < low ? low : value > high ? high : value;
}

static uint8_t channel(uint32_t color, int shift, float scale)
{
  return (uint8_t)clampf(((color >> shift) & 255u) * scale, 0.0f, 255.0f);
}

static uint32_t shade(uint32_t color, float scale)
{
  return ((uint32_t)channel(color, 16, scale) << 16) |
         ((uint32_t)channel(color, 8, scale) << 8) | channel(color, 0, scale);
}

static uint32_t mix(uint32_t a, uint32_t b, float amount)
{
  uint32_t out = 0;
  int shift;

  amount = clampf(amount, 0.0f, 1.0f);
  for (shift = 0; shift <= 16; shift += 8)
    {
      float av = (a >> shift) & 255u;
      float bv = (b >> shift) & 255u;
      out |= (uint32_t)(av + (bv - av) * amount) << shift;
    }

  return out;
}

static uint32_t iris_color(uint32_t color, float radius)
{
  if (radius < 0.55f)
    {
      return mix(shade(color, 1.25f), color, radius / 0.55f);
    }

  if (radius < 0.85f)
    {
      return mix(color, shade(color, 0.55f), (radius - 0.55f) / 0.30f);
    }

  return mix(shade(color, 0.55f), shade(color, 0.30f),
             (radius - 0.85f) / 0.15f);
}

static void pixel(struct nyabula_eye_renderer_s *r, int x, int y,
                  uint32_t color, float opacity)
{
  lv_color32_t *p;
  uint32_t stride;
  float inverse;

  if ((unsigned int)x >= W || (unsigned int)y >= H || opacity <= 0.0f)
    {
      return;
    }

  if (r->clip_lids && r->clip_eye != NULL &&
      !in_lids(r->clip_eye, x + 0.5f, y + 0.5f))
    {
      return;
    }

  stride = r->draw->header.stride / sizeof(lv_color32_t);
  p = &((lv_color32_t *)r->draw->data)[y * stride + x];
  opacity = clampf(opacity, 0.0f, 1.0f);
  inverse = 1.0f - opacity;
  p->red = p->red * inverse + ((color >> 16) & 255u) * opacity;
  p->green = p->green * inverse + ((color >> 8) & 255u) * opacity;
  p->blue = p->blue * inverse + (color & 255u) * opacity;
  p->alpha = 255;
}

static void to_screen(const struct transform_s *t, float x, float y, float *sx,
                      float *sy)
{
  y *= t->sy;
  *sx = t->cx + x * t->cs - y * t->sn;
  *sy = t->cy + x * t->sn + y * t->cs;
}

static void to_local(const struct transform_s *t, float x, float y, float *lx,
                     float *ly)
{
  float dx = x - t->cx;
  float dy = y - t->cy;
  *lx = dx * t->cs + dy * t->sn;
  *ly = (-dx * t->sn + dy * t->cs) / t->sy;
}

static bool in_screen(const struct eye_s *e, float x, float y)
{
  float dx = x - e->t.cx;
  float dy = y - e->t.cy;
  return dx * dx + dy * dy <= R * R;
}

static int text_width(const lv_font_t *font, const char *text)
{
  lv_font_glyph_dsc_t descriptor;
  uint32_t offset = 0;
  int width = 0;

  while (text[offset] != '\0')
    {
      uint32_t next_offset;
      uint32_t codepoint = utf8_next(text, &offset);
      uint32_t next;
      next_offset = offset;
      next = text[next_offset] == '\0' ? 0 : utf8_next(text, &next_offset);
      if (lv_font_get_glyph_dsc(font, &descriptor, codepoint, next))
        {
          width += descriptor.adv_w;
        }
    }

  return width;
}

static void text_center_at(struct nyabula_eye_renderer_s *r,
                           const struct eye_s *e, const lv_font_t *font,
                           const char *text, float center_x, float center_y,
                           uint32_t color, float opacity)
{
  lv_font_glyph_dsc_t descriptor;
  uint32_t offset = 0;
  float pen_x;
  float top;

  font = renderer_font(r, font);
  pen_x = -text_width(font, text) * 0.5f;
  top = center_y - font->line_height * 0.5f;

  while (text[offset] != '\0')
    {
      uint32_t next_offset;
      uint32_t codepoint = utf8_next(text, &offset);
      uint32_t next;
      const lv_draw_buf_t *bitmap;
      uint32_t stride;
      int glyph_x;
      int glyph_y;

      next_offset = offset;
      next = text[next_offset] == '\0' ? 0 : utf8_next(text, &next_offset);
      if (!lv_font_get_glyph_dsc(font, &descriptor, codepoint, next))
        {
          continue;
        }

      bitmap = descriptor.box_w == 0 || descriptor.box_h == 0
                   ? NULL
                   : lv_font_get_glyph_bitmap(&descriptor, r->glyph);
      if (bitmap != NULL)
        {
          float origin_x = pen_x + descriptor.ofs_x;
          float origin_y = top + font->line_height - font->base_line -
                           descriptor.box_h - descriptor.ofs_y;
          stride = lv_draw_buf_width_to_stride(descriptor.box_w,
                                               LV_COLOR_FORMAT_A8);
          for (glyph_y = 0; glyph_y < descriptor.box_h; glyph_y++)
            {
              const uint8_t *row =
                  (const uint8_t *)bitmap->data + glyph_y * stride;
              for (glyph_x = 0; glyph_x < descriptor.box_w; glyph_x++)
                {
                  float alpha = row[glyph_x] / 255.0f * opacity;
                  if (alpha > 0.0f)
                    {
                       pixel(r, roundf(e->t.cx + center_x + origin_x + glyph_x),
                            roundf(e->t.cy + origin_y + glyph_y), color,
                            alpha);
                    }
                }
            }
        }

      lv_font_glyph_release_draw_data(&descriptor);
      pen_x += descriptor.adv_w;
    }
}

static void text_center(struct nyabula_eye_renderer_s *r,
                        const struct eye_s *e, const lv_font_t *font,
                        const char *text, float center_y, uint32_t color,
                        float opacity)
{
  text_center_at(r, e, font, text, 0.0f, center_y, color, opacity);
}

static float cubic(float a, float b, float c, float d, float t)
{
  float u = 1.0f - t;
  return u * u * u * a + 3.0f * u * u * t * b + 3.0f * u * t * t * c +
         t * t * t * d;
}

static float lid_y(float x, float extent, float left, float right, float bulge,
                   bool top)
{
  float x0 = top ? extent * 1.3f : -extent * 1.3f;
  float x1 = top ? extent * 0.4f : -extent * 0.45f;
  float x2 = top ? -extent * 0.4f : extent * 0.45f;
  float x3 = top ? -extent * 1.3f : extent * 1.3f;
  float y0 = top ? right : left;
  float y3 = top ? left : right;
  float low = 0.0f;
  float high = 1.0f;
  int i;

  for (i = 0; i < 9; i++)
    {
      float mid = (low + high) * 0.5f;
      bool forward = top ? cubic(x0, x1, x2, x3, mid) > x
                         : cubic(x0, x1, x2, x3, mid) < x;
      if (forward)
        {
          low = mid;
        }
      else
        {
          high = mid;
        }
    }

  return cubic(y0, y0 + bulge, y3 + bulge, y3, (low + high) * 0.5f);
}

static bool in_lids(const struct eye_s *e, float sx, float sy)
{
  float x;
  float y;
  int index;

  to_local(&e->t, sx, sy, &x, &y);
  index = (int)roundf(x) + LID_OFFSET;
  if ((unsigned int)index >= LID_SAMPLES)
    {
      return true;
    }

  return y <= e->top_y[index] || y >= e->bottom_y[index];
}

static void disk(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                 float x, float y, float radius, uint32_t color, float opacity,
                 bool lid_clip)
{
  int px;
  int py;
  for (py = floorf(y - radius - 1); py <= ceilf(y + radius + 1); py++)
    {
      for (px = floorf(x - radius - 1); px <= ceilf(x + radius + 1); px++)
        {
          float dx = px + 0.5f - x;
          float dy = py + 0.5f - y;
          float d = sqrtf(dx * dx + dy * dy);
          if (d <= radius + 0.5f && in_screen(e, px + 0.5f, py + 0.5f) &&
              (!lid_clip || in_lids(e, px + 0.5f, py + 0.5f)))
            {
              pixel(r, px, py, color,
                    opacity * clampf(radius + 0.5f - d, 0.0f, 1.0f));
            }
        }
    }
}

static void line(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                 float x1, float y1, float x2, float y2, float width,
                 uint32_t color, float opacity, bool lid_clip)
{
  float sx1;
  float sy1;
  float sx2;
  float sy2;
  int steps;
  int i;

  to_screen(&e->t, x1, y1, &sx1, &sy1);
  to_screen(&e->t, x2, y2, &sx2, &sy2);
  steps = ceilf(fmaxf(fabsf(sx2 - sx1), fabsf(sy2 - sy1)) * 1.25f);
  steps = steps < 1 ? 1 : steps;
  for (i = 0; i <= steps; i++)
    {
      float amount = (float)i / steps;
      disk(r, e, sx1 + (sx2 - sx1) * amount, sy1 + (sy2 - sy1) * amount,
           width * 0.5f, color, opacity, lid_clip);
    }
}

static void arc(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                float cx, float cy, float radius, float start, float sweep,
                float width, uint32_t color, float opacity)
{
  int steps = fmaxf(2.0f, ceilf(fabsf(sweep) * radius));
  int i;
  for (i = 0; i < steps; i++)
    {
      float a = start + sweep * (float)i / steps;
      float b = start + sweep * (float)(i + 1) / steps;
      line(r, e, cx + cosf(a) * radius, cy + sinf(a) * radius,
           cx + cosf(b) * radius, cy + sinf(b) * radius, width, color, opacity,
           false);
    }
}

static void base(struct nyabula_eye_renderer_s *r, const struct eye_s *e)
{
  int x;
  int y;
  for (y = e->t.cy - R; y <= e->t.cy + R; y++)
    {
      for (x = e->t.cx - R; x <= e->t.cx + R; x++)
        {
          float lx;
          float ly;
          float dx = x + 0.5f - e->t.cx;
          float dy = y + 0.5f - e->t.cy;
          float screen_radius = sqrtf(dx * dx + dy * dy);
          float iradius;
          if (screen_radius > R)
            {
              continue;
            }

          to_local(&e->t, x + 0.5f, y + 0.5f, &lx, &ly);
          pixel(r, x, y, e->f->iris_rgb,
                e->f->glow * 0.175f *
                    (1.0f - clampf((hypotf(lx, ly) - R * 0.4f) / (R * 1.5f),
                                   0.0f, 1.0f)));
          iradius = hypotf(lx - e->gx * 0.5f, ly - e->gy * 0.5f) / e->ir;
          if (iradius <= 1.0f)
            {
              pixel(r, x, y, iris_color(e->f->iris_rgb, iradius), 1.0f);
            }
        }
    }
}

static void iris(struct nyabula_eye_renderer_s *r, const struct eye_s *e)
{
  int i;
  for (i = 0; i < FIBERS; i++)
    {
      float a = (float)i / FIBERS * PI * 2.0f + sinf(i * 7.0f) * 0.1f;
      float r1 = e->ir * (0.28f + (float)((i * 37) % 13) / 13.0f * 0.15f);
      float r2 = e->ir * (0.82f + (float)((i * 53) % 7) / 7.0f * 0.14f);
      line(r, e, e->gx + cosf(a) * r1, e->gy + sinf(a) * r1,
           e->gx * 0.3f + cosf(a) * r2, e->gy * 0.3f + sinf(a) * r2, 1.0f,
           shade(e->f->iris_rgb, 1.6f), 0.16f, false);
    }
}

static void ellipse(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                    float cx, float cy, float rx, float ry, float rotation,
                    uint32_t color, float opacity)
{
  float center_x;
  float center_y;
  float ux;
  float uy;
  float vx;
  float vy;
  float extent_x;
  float extent_y;
  int max_x;
  int max_y;
  int min_x;
  int min_y;
  int x;
  int y;
  float sn = sinf(rotation);
  float cs = cosf(rotation);

  to_screen(&e->t, cx, cy, &center_x, &center_y);
  ux = rx * (cs * e->t.cs - sn * e->t.sy * e->t.sn);
  uy = rx * (cs * e->t.sn + sn * e->t.sy * e->t.cs);
  vx = ry * (-sn * e->t.cs - cs * e->t.sy * e->t.sn);
  vy = ry * (-sn * e->t.sn + cs * e->t.sy * e->t.cs);
  extent_x = sqrtf(ux * ux + vx * vx) + 1.0f;
  extent_y = sqrtf(uy * uy + vy * vy) + 1.0f;
  min_x = (int)floorf(center_x - extent_x);
  max_x = (int)ceilf(center_x + extent_x);
  min_y = (int)floorf(center_y - extent_y);
  max_y = (int)ceilf(center_y + extent_y);

  for (y = min_y; y <= max_y; y++)
    {
      for (x = min_x; x <= max_x; x++)
        {
          float lx;
          float ly;
          float dx;
          float dy;
          float tx;
          float ty;
          if (!in_screen(e, x + 0.5f, y + 0.5f))
            {
              continue;
            }

          to_local(&e->t, x + 0.5f, y + 0.5f, &lx, &ly);
          dx = lx - cx;
          dy = ly - cy;
          tx = dx * cs + dy * sn;
          ty = -dx * sn + dy * cs;
          if (tx * tx / (rx * rx) + ty * ty / (ry * ry) <= 1.0f)
            {
              pixel(r, x, y, color, opacity);
            }
        }
    }
}

static void pupil(struct nyabula_eye_renderer_s *r, const struct eye_s *e)
{
  float rx = e->ir * e->f->pupil_width * 0.80f;
  float ry = e->ir * e->f->pupil_height * 0.84f;
  if (rx > 0.01f && ry > 0.01f)
    {
      ellipse(r, e, e->gx, e->gy, rx, ry, 0.0f, 0x05070a, 1.0f);
      ellipse(r, e, e->gx, e->gy, rx, ry, 0.0f, shade(e->f->iris_rgb, 1.7f),
              0.12f);
      ellipse(r, e, e->gx, e->gy, rx * 0.96f, ry * 0.96f, 0.0f, 0x05070a,
              1.0f);
    }
}

static void star(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                 float cx, float cy, float radius, float rotation,
                 uint32_t color, float opacity)
{
  float points[20];
  int x;
  int y;
  int i;

  for (i = 0; i < 10; i++)
    {
      float a = rotation + i * PI / 5.0f;
      float pr = (i & 1) ? radius * 0.45f : radius;
      points[i * 2] = cx + cosf(a) * pr;
      points[i * 2 + 1] = cy + sinf(a) * pr;
    }

  for (y = e->t.cy - R; y <= e->t.cy + R; y++)
    {
      for (x = e->t.cx - R; x <= e->t.cx + R; x++)
        {
          float lx;
          float ly;
          bool inside = false;
          int previous = 9;
          if (!in_screen(e, x + 0.5f, y + 0.5f))
            {
              continue;
            }

          to_local(&e->t, x + 0.5f, y + 0.5f, &lx, &ly);
          for (i = 0; i < 10; i++)
            {
              float x1 = points[i * 2];
              float y1 = points[i * 2 + 1];
              float x2 = points[previous * 2];
              float y2 = points[previous * 2 + 1];
              if ((y1 > ly) != (y2 > ly) &&
                  lx < (x2 - x1) * (ly - y1) / (y2 - y1) + x1)
                {
                  inside = !inside;
                }

              previous = i;
            }

          if (inside)
            {
              pixel(r, x, y, color, opacity);
            }
        }
    }
}

static void heart(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                  float cx, float cy, float radius, uint32_t color,
                  float opacity)
{
  int x;
  int y;

  for (y = e->t.cy - R; y <= e->t.cy + R; y++)
    {
      for (x = e->t.cx - R; x <= e->t.cx + R; x++)
        {
          float lx;
          float ly;
          float nx;
          float ny;
          float q;
          if (!in_screen(e, x + 0.5f, y + 0.5f))
            {
              continue;
            }

          to_local(&e->t, x + 0.5f, y + 0.5f, &lx, &ly);
          nx = (lx - cx) / radius * 1.15f;
          ny = -(ly - cy) / radius * 1.15f;
          q = nx * nx + ny * ny - 1.0f;
          if (q * q * q - nx * nx * ny * ny * ny <= 0.0f)
            {
              pixel(r, x, y, color, opacity);
            }
        }
    }
}

static bool icon_contains(const struct nyabula_eye_icon_s *icon, float x,
                          float y)
{
  int winding = 0;
  int contour;

  for (contour = 0; contour < icon->contour_count; contour++)
    {
      int first = icon->contours[contour];
      int limit = icon->contours[contour + 1];
      int previous = limit - 1;
      int index;

      for (index = first; index < limit; index++)
        {
          const struct nyabula_eye_icon_point_s *a = &icon->points[index];
          const struct nyabula_eye_icon_point_s *b = &icon->points[previous];
          float cross = (float)(a->x - b->x) * (y - b->y) -
                        (x - b->x) * (float)(a->y - b->y);

          if (b->y <= y && a->y > y && cross > 0.0f)
            {
              winding++;
            }
          else if (b->y > y && a->y <= y && cross < 0.0f)
            {
              winding--;
            }

          previous = index;
        }
    }

  return winding != 0;
}

static struct icon_cache_s *icon_cache_get(
    struct nyabula_eye_renderer_s *r,
    const struct nyabula_eye_icon_s *asset, float size)
{
  struct icon_cache_s *entry = NULL;
  uint32_t oldest = UINT32_MAX;
  int size_key = lroundf(size * R);
  int index;

  size = size_key / R;
  r->icon_cache_clock++;

  for (index = 0; index < ICON_CACHE_COUNT; index++)
    {
      if (r->icon_cache[index].asset == asset &&
          r->icon_cache[index].size_key == size_key)
        {
          r->icon_cache[index].last_used = r->icon_cache_clock;
          return &r->icon_cache[index];
        }

      if (entry == NULL && r->icon_cache[index].asset == NULL)
        {
          entry = &r->icon_cache[index];
        }
      else if (entry == NULL || r->icon_cache[index].last_used < oldest)
        {
          oldest = r->icon_cache[index].last_used;
        }
    }

  if (entry == NULL)
    {
      for (index = 0; index < ICON_CACHE_COUNT; index++)
        {
          if (r->icon_cache[index].last_used == oldest)
            {
              entry = &r->icon_cache[index];
              free(entry->coverage);
              memset(entry, 0, sizeof(*entry));
              break;
            }
        }
    }

  if (entry != NULL)
    {
      float scale = R * size / asset->width;
      float half_width = asset->width * 0.5f;
      float half_height = asset->height * 0.5f;
      float extent_x = R * size * 0.5f;
      float extent_y = extent_x * asset->height / asset->width;
      int x;
      int y;

      entry->left = floorf(-extent_x) - 2;
      entry->top = floorf(-extent_y) - 2;
      entry->width = ceilf(extent_x) - entry->left + 3;
      entry->height = ceilf(extent_y) - entry->top + 3;
      entry->coverage = calloc((size_t)entry->width * entry->height, 1);
      if (entry->coverage == NULL)
        {
          return NULL;
        }

      for (y = 0; y < entry->height; y++)
        {
          for (x = 0; x < entry->width; x++)
            {
              int sample_x;
              int sample_y;
              int inside = 0;

              for (sample_y = 0; sample_y < 4; sample_y++)
                {
                  for (sample_x = 0; sample_x < 4; sample_x++)
                    {
                      float local_x = entry->left + x +
                                      (sample_x + 0.5f) * 0.25f;
                      float local_y = entry->top + y +
                                      (sample_y + 0.5f) * 0.25f;
                      float icon_x = local_x / scale + half_width;
                      float icon_y = local_y / scale + half_height;

                      inside += icon_contains(asset, icon_x, icon_y);
                    }
                }

              entry->coverage[y * entry->width + x] =
                  (uint8_t)((inside * 255 + 8) / 16);
            }
        }

      entry->asset = asset;
      entry->size_key = size_key;
      entry->last_used = r->icon_cache_clock;
    }

  return entry;
}

static void icon_fill(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                      const struct nyabula_eye_icon_s *icon, float size,
                      uint32_t color, float opacity)
{
  float raster_size = strcmp(icon->name, "star") == 0
                          ? 0.28f
                      : strcmp(icon->name, "subwoofer") == 0
                          ? 1.21f
                      : strcmp(icon->name, "task_confirm") == 0
                          ? 1.07f
                          : size;
  struct icon_cache_s *cache = icon_cache_get(r, icon, raster_size);
  float sample_scale = raster_size / size;
  int min_x = e->t.cx - R * size * 0.55f - 2;
  int max_x = e->t.cx + R * size * 0.55f + 2;
  int min_y = e->t.cy - R * size * 0.55f - 2;
  int max_y = e->t.cy + R * size * 0.55f + 2;
  int x;
  int y;

  if (cache == NULL)
    {
      return;
    }

  for (y = min_y; y <= max_y; y++)
    {
      for (x = min_x; x <= max_x; x++)
        {
          float local_x;
          float local_y;
          int mask_x;
          int mask_y;
          uint8_t coverage;

          if (!in_screen(e, x + 0.5f, y + 0.5f))
            {
              continue;
            }

          to_local(&e->t, x + 0.5f, y + 0.5f, &local_x, &local_y);
          mask_x = floorf(local_x * sample_scale) - cache->left;
          mask_y = floorf(local_y * sample_scale) - cache->top;
          if (mask_x < 0 || mask_x >= cache->width || mask_y < 0 ||
              mask_y >= cache->height)
            {
              continue;
            }

          coverage = cache->coverage[mask_y * cache->width + mask_x];
          if (coverage != 0)
            {
              pixel(r, x, y, color, opacity * coverage / 255.0f);
            }
        }
    }
}

static void icon_stroke(struct nyabula_eye_renderer_s *r,
                        const struct eye_s *e,
                        const struct nyabula_eye_icon_s *icon, float size,
                        uint32_t color, float opacity, float reveal)
{
  float scale = R * size / icon->width;
  float half_width = icon->width * 0.5f;
  float half_height = icon->height * 0.5f;
  float total = 0.0f;
  float visible;
  float walked = 0.0f;
  int contour;

  for (contour = 0; contour < icon->contour_count; contour++)
    {
      int index;
      for (index = icon->contours[contour] + 1;
           index < icon->contours[contour + 1]; index++)
        {
          float dx = icon->points[index].x - icon->points[index - 1].x;
          float dy = icon->points[index].y - icon->points[index - 1].y;
          total += hypotf(dx, dy);
        }
    }

  visible = total * clampf(reveal, 0.0f, 1.0f);
  for (contour = 0; contour < icon->contour_count && walked < visible;
       contour++)
    {
      int index;
      for (index = icon->contours[contour] + 1;
           index < icon->contours[contour + 1] && walked < visible; index++)
        {
          const struct nyabula_eye_icon_point_s *a = &icon->points[index - 1];
          const struct nyabula_eye_icon_point_s *b = &icon->points[index];
          float dx = b->x - a->x;
          float dy = b->y - a->y;
          float length = hypotf(dx, dy);
          float part = clampf((visible - walked) / length, 0.0f, 1.0f);
          float x1 = (a->x - half_width) * scale;
          float y1 = (a->y - half_height) * scale;
          float x2 = (a->x + dx * part - half_width) * scale;
          float y2 = (a->y + dy * part - half_height) * scale;
          line(r, e, x1, y1, x2, y2, R * 0.028f, color, opacity, false);
          walked += length;
        }
    }
}

static void icon(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                 const char *name, float size, uint32_t color, float opacity,
                 float reveal)
{
  const struct nyabula_eye_icon_s *asset = nyabula_eye_icon_find(name);
  float fill = clampf((reveal - 0.46f) / 0.54f, 0.0f, 1.0f);

  if (asset == NULL)
    {
      return;
    }

  icon_stroke(r, e, asset, size, color,
              opacity * (0.88f - fill * 0.68f), reveal);
  if (fill > 0.0f)
    {
      icon_fill(r, e, asset, size, color, opacity * 0.92f * fill);
    }
}

static void icon_at(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                    const char *name, float size, float x, float y,
                    float rotation, uint32_t color, float opacity,
                    float reveal)
{
  struct eye_s placed = *e;
  float cosine = cosf(rotation);
  float sine = sinf(rotation);
  float old_cosine = e->t.cs;
  float old_sine = e->t.sn;

  to_screen(&e->t, x, y, &placed.t.cx, &placed.t.cy);
  placed.t.cs = old_cosine * cosine - old_sine * sine;
  placed.t.sn = old_sine * cosine + old_cosine * sine;
  icon(r, &placed, name, size, color, opacity, reveal);
}

static void icon_group_at(struct nyabula_eye_renderer_s *r,
                          const struct eye_s *e, const char *name,
                          float size, float x, float y, float rotation,
                          uint32_t color, float opacity, float reveal)
{
  const struct nyabula_eye_icon_s *asset = nyabula_eye_icon_find(name);
  struct eye_s placed = *e;
  float cosine = cosf(rotation);
  float sine = sinf(rotation);
  float old_cosine = e->t.cs;
  float old_sine = e->t.sn;
  float fill = clampf((reveal - 0.46f) / 0.54f, 0.0f, 1.0f);

  if (asset == NULL)
    {
      return;
    }

  to_screen(&e->t, x, y, &placed.t.cx, &placed.t.cy);
  placed.t.cs = old_cosine * cosine - old_sine * sine;
  placed.t.sn = old_sine * cosine + old_cosine * sine;

  if (fill < 1.0f)
    {
      icon_stroke(r, &placed, asset, size, color,
                  opacity * (1.0f - fill), reveal);
    }

  if (fill > 0.0f)
    {
      icon_fill(r, &placed, asset, size, color, opacity * fill);
    }
}

static void icon_outline(struct nyabula_eye_renderer_s *r,
                         const struct eye_s *e, const char *name, float size,
                         float width, uint32_t color, float opacity)
{
  const struct nyabula_eye_icon_s *asset = nyabula_eye_icon_find(name);
  float scale;
  float half_width;
  float half_height;
  int contour;

  if (asset == NULL)
    {
      return;
    }

  scale = R * size / asset->width;
  half_width = asset->width * 0.5f;
  half_height = asset->height * 0.5f;
  for (contour = 0; contour < asset->contour_count; contour++)
    {
      int index;
      for (index = asset->contours[contour] + 1;
           index < asset->contours[contour + 1]; index++)
        {
          const struct nyabula_eye_icon_point_s *a = &asset->points[index - 1];
          const struct nyabula_eye_icon_point_s *b = &asset->points[index];
          line(r, e, (a->x - half_width) * scale,
               (a->y - half_height) * scale,
               (b->x - half_width) * scale,
               (b->y - half_height) * scale, width, color, opacity, false);
        }
    }
}

static void filled_rect(struct nyabula_eye_renderer_s *r,
                        const struct eye_s *e, float left, float top,
                        float right, float bottom, uint32_t color,
                        float opacity)
{
  int x;
  int y;

  for (y = floorf(e->t.cy + top); y <= ceilf(e->t.cy + bottom); y++)
    {
      for (x = floorf(e->t.cx + left); x <= ceilf(e->t.cx + right); x++)
        {
          if (in_screen(e, x + 0.5f, y + 0.5f))
            {
              pixel(r, x, y, color, opacity);
            }
        }
    }
}

static const char *scene_text(const char *value, const char *fallback)
{
  return value[0] == '\0' ? fallback : value;
}

static uint32_t scene_color(const struct eye_s *e)
{
  return shade(e->f->iris_rgb, 1.75f);
}

static void scene_backdrop(struct nyabula_eye_renderer_s *r,
                           const struct eye_s *e, bool minimal, float opacity)
{
  float center_x;
  float center_y;

  if (minimal)
    {
      return;
    }

  to_screen(&e->t, 0.0f, 0.0f, &center_x, &center_y);
  disk(r, e, center_x, center_y, R * 0.93f, 0x000307, opacity, false);
}

static void scene_dial(struct nyabula_eye_renderer_s *r,
                       const struct eye_s *e, uint32_t color, float progress,
                       float opacity)
{
  arc(r, e, 0.0f, 0.0f, R * 0.70f, -PI * 0.5f, PI * 2.0f,
      R * 0.035f, color, opacity * 0.17f);
  if (progress > 0.0f)
    {
      arc(r, e, 0.0f, 0.0f, R * 0.70f, -PI * 0.5f,
          clampf(progress, 0.0f, 1.0f) * PI * 2.0f, R * 0.035f,
          color, opacity * 0.82f);
    }
}

static void rounded_rect_outline(struct nyabula_eye_renderer_s *r,
                                 const struct eye_s *e, float left,
                                 float top, float right, float bottom,
                                 float radius, float width, uint32_t color,
                                 float opacity)
{
  line(r, e, left + radius, top, right - radius, top, width, color, opacity,
       false);
  line(r, e, left + radius, bottom, right - radius, bottom, width, color,
       opacity, false);
  line(r, e, left, top + radius, left, bottom - radius, width, color, opacity,
       false);
  line(r, e, right, top + radius, right, bottom - radius, width, color,
       opacity, false);
  arc(r, e, left + radius, top + radius, radius, PI, PI * 0.5f, width, color,
      opacity);
  arc(r, e, right - radius, top + radius, radius, -PI * 0.5f, PI * 0.5f,
      width, color, opacity);
  arc(r, e, right - radius, bottom - radius, radius, 0.0f, PI * 0.5f, width,
      color, opacity);
  arc(r, e, left + radius, bottom - radius, radius, PI * 0.5f, PI * 0.5f,
      width, color, opacity);
}

static void scene_eq_curve(struct nyabula_eye_renderer_s *r,
                           const struct eye_s *e, uint32_t color,
                           float seconds, float opacity, bool calibrating)
{
  float previous_x = -R * 0.62f;
  float previous_y = R * -0.04f;
  int index;

  for (index = -2; index <= 2; index++)
    {
      line(r, e, -R * 0.62f, index * R * 0.16f, R * 0.62f,
           index * R * 0.16f, R * 0.010f, color, opacity * 0.14f, false);
    }

  for (index = 1; index <= 48; index++)
    {
      float x = R * (-0.62f + index / 48.0f * 1.24f);
      float nx = x / R;
      float y = R * (-0.04f -
                     expf(-powf((nx + 0.28f) * 4.0f, 2.0f)) * 0.16f +
                     expf(-powf((nx - 0.22f) * 5.0f, 2.0f)) * 0.11f);
      line(r, e, previous_x, previous_y, x, y, R * 0.025f, color,
           opacity * 0.88f, false);
      previous_x = x;
      previous_y = y;
    }

  if (calibrating)
    {
      float sweep = fmodf(seconds * 0.20f, 1.0f) * R * 1.24f - R * 0.62f;
      for (index = 0; index < 10; index++)
        {
          float amount = (index + 1) / 10.0f;
          filled_rect(r, e, sweep - R * 0.15f + index * R * 0.015f,
                      -R * 0.42f,
                      sweep - R * 0.15f + (index + 1) * R * 0.015f,
                      R * 0.42f, color, opacity * 0.34f * amount);
        }
    }
}

static void scene_play_triangle(struct nyabula_eye_renderer_s *r,
                                const struct eye_s *e, uint32_t color,
                                float opacity)
{
  int y;
  int top = (int)floorf(-R * 0.27f);
  int middle = (int)lroundf(-R * 0.08f);
  int bottom = (int)ceilf(R * 0.11f);
  float left = -R * 0.105f;
  float tip = R * 0.205f;

  for (y = top; y <= bottom; y++)
    {
      float progress;
      float right;

      if (y <= middle)
        {
          progress = (float)(y - top) / (float)(middle - top);
        }
      else
        {
          progress = (float)(bottom - y) / (float)(bottom - middle);
        }

      right = left + (tip - left) * clampf(progress, 0.0f, 1.0f);
      filled_rect(r, e, left, y, right, y + 1.0f, color, opacity);
    }
}

struct scene_point_s
{
  float x;
  float y;
};

static void scene_append_cubic(struct scene_point_s *points, int *count,
                               struct scene_point_s start,
                               struct scene_point_s control1,
                               struct scene_point_s control2,
                               struct scene_point_s end)
{
  int index;

  for (index = 1; index <= 12; index++)
    {
      float t = index / 12.0f;
      float u = 1.0f - t;
      points[*count].x = u * u * u * start.x +
                         3.0f * u * u * t * control1.x +
                         3.0f * u * t * t * control2.x + t * t * t * end.x;
      points[*count].y = u * u * u * start.y +
                         3.0f * u * u * t * control1.y +
                         3.0f * u * t * t * control2.y + t * t * t * end.y;
      (*count)++;
    }
}

static bool scene_polygon_contains(const struct scene_point_s *points,
                                   int count, float x, float y)
{
  bool inside = false;
  int previous = count - 1;
  int index;

  for (index = 0; index < count; index++)
    {
      if ((points[index].y > y) != (points[previous].y > y) &&
          x < (points[previous].x - points[index].x) *
                      (y - points[index].y) /
                      (points[previous].y - points[index].y) +
                  points[index].x)
        {
          inside = !inside;
        }

      previous = index;
    }

  return inside;
}

static void scene_fill_polygon(struct nyabula_eye_renderer_s *r,
                               const struct eye_s *e,
                               const struct scene_point_s *points, int count,
                               uint32_t top_color, uint32_t bottom_color,
                               float opacity)
{
  float min_x = points[0].x;
  float max_x = points[0].x;
  float min_y = points[0].y;
  float max_y = points[0].y;
  int index;
  int x;
  int y;

  for (index = 1; index < count; index++)
    {
      min_x = fminf(min_x, points[index].x);
      max_x = fmaxf(max_x, points[index].x);
      min_y = fminf(min_y, points[index].y);
      max_y = fmaxf(max_y, points[index].y);
    }

  for (y = floorf(e->t.cy + min_y); y <= ceilf(e->t.cy + max_y); y++)
    {
      for (x = floorf(e->t.cx + min_x); x <= ceilf(e->t.cx + max_x); x++)
        {
          float local_x;
          float local_y;
          float amount;

          if (!in_screen(e, x + 0.5f, y + 0.5f))
            {
              continue;
            }

          to_local(&e->t, x + 0.5f, y + 0.5f, &local_x, &local_y);
          if (!scene_polygon_contains(points, count, local_x, local_y))
            {
              continue;
            }

          amount = clampf((local_y - min_y) / fmaxf(max_y - min_y, 1.0f),
                          0.0f, 1.0f);
          pixel(r, x, y, mix(top_color, bottom_color, amount), opacity);
        }
    }
}

static void scene_stroke_points(struct nyabula_eye_renderer_s *r,
                                const struct eye_s *e,
                                const struct scene_point_s *points, int count,
                                float width, uint32_t color, float opacity)
{
  int index;

  for (index = 1; index < count; index++)
    {
      line(r, e, points[index - 1].x, points[index - 1].y, points[index].x,
           points[index].y, width, color, opacity, false);
    }
}

static int scene_cloud_points(struct scene_point_s *points, float drift,
                              float offset_y, float scale)
{
  struct scene_point_s current = {-R * 0.46f, R * 0.18f};
  int count = 0;

#define CLOUD_POINT(x, y) \
  ((struct scene_point_s){drift + R * (x) * scale, \
                          offset_y + R * (y) * scale})
#define CLOUD_CUBIC(c1x, c1y, c2x, c2y, ex, ey) \
  do \
    { \
      struct scene_point_s end = CLOUD_POINT(ex, ey); \
      scene_append_cubic(points, &count, current, \
                         CLOUD_POINT(c1x, c1y), CLOUD_POINT(c2x, c2y), end); \
      current = end; \
    } \
  while (0)

  current = CLOUD_POINT(-0.46f, 0.18f);
  points[count++] = current;
  CLOUD_CUBIC(-0.49f, 0.05f, -0.43f, -0.15f, -0.24f, -0.18f);
  CLOUD_CUBIC(-0.18f, -0.43f, 0.13f, -0.48f, 0.24f, -0.22f);
  CLOUD_CUBIC(0.40f, -0.21f, 0.49f, -0.08f, 0.48f, 0.09f);
  CLOUD_CUBIC(0.48f, 0.18f, 0.41f, 0.22f, 0.30f, 0.22f);
  points[count++] = CLOUD_POINT(-0.39f, 0.22f);
  CLOUD_CUBIC(-0.43f, 0.22f, -0.46f, 0.21f, -0.46f, 0.18f);

#undef CLOUD_CUBIC
#undef CLOUD_POINT
  return count;
}

static void scene_draw_cloud(struct nyabula_eye_renderer_s *r,
                             const struct eye_s *e, uint32_t color,
                             float drift, float offset_y, float scale,
                             float opacity)
{
  struct scene_point_s points[80];
  int count = scene_cloud_points(points, drift + R * 0.035f * scale,
                                 offset_y + R * 0.035f * scale, scale);

  scene_fill_polygon(r, e, points, count, shade(color, 0.38f),
                     shade(color, 0.38f), opacity * 0.55f);
  count = scene_cloud_points(points, drift, offset_y, scale);
  scene_fill_polygon(r, e, points, count, shade(color, 1.06f),
                     shade(color, 0.72f), opacity * 0.96f);
  scene_stroke_points(r, e, points, count, R * 0.014f,
                      shade(color, 1.22f), opacity * 0.28f);
}

static void scene_music(struct nyabula_eye_renderer_s *r,
                        const struct eye_s *e,
                        const struct nyabula_eye_scene_payload_s *payload,
                        float seconds, float opacity)
{
  uint32_t color = scene_color(e);
  float duration = payload->duration_ms == 0 ? 235.0f
                                             : payload->duration_ms / 1000.0f;
  float position = payload->position_ms == 0 ? fmodf(seconds, duration)
                                              : payload->position_ms / 1000.0f;
  float progress = clampf(position / duration, 0.0f, 1.0f);
  char value[32];

  if (e->id == NYABULA_EYE_LEFT)
    {
      arc(r, e, 0.0f, 0.0f, R * 0.89f, -PI * 0.5f, PI * 2.0f,
          R * 0.032f, shade(color, 0.38f), opacity * 0.12f);
      arc(r, e, 0.0f, 0.0f, R * 0.89f, -PI * 0.5f,
          progress * PI * 2.0f, R * 0.032f, color, opacity * 0.92f);
      ellipse(r, e,
              cosf(-PI * 0.5f + progress * PI * 2.0f) * R * 0.89f,
              sinf(-PI * 0.5f + progress * PI * 2.0f) * R * 0.89f,
              R * 0.024f, R * 0.024f, 0.0f, color, opacity * 0.95f);
      if (payload->playing)
        {
          scene_play_triangle(r, e, color, opacity * 0.94f);
        }
      else
        {
          line(r, e, -R * 0.09f, -R * 0.20f, -R * 0.09f, R * 0.20f,
               R * 0.065f, color, opacity * 0.94f, false);
          line(r, e, R * 0.09f, -R * 0.20f, R * 0.09f, R * 0.20f,
               R * 0.065f, color, opacity * 0.94f, false);
        }
      text_center(r, e,
                  scene_font(r, FONT_FAMILY_ENGLISH, 14,
                             &nyabula_font_english_18),
                  scene_text(payload->title, "NYABULA MIX"), R * 0.36f,
                  color, opacity * 0.50f);
      snprintf(value, sizeof(value), "%02u:%02u  /  %02u:%02u",
               (unsigned int)position / 60, (unsigned int)position % 60,
               (unsigned int)duration / 60, (unsigned int)duration % 60);
      text_center(r, e,
                  scene_font(r, FONT_FAMILY_ENGLISH, 16,
                             &nyabula_font_english_18),
                  value, R * 0.49f, color, opacity * 0.76f);
    }
  else if (payload->music_view == NYABULA_EYE_MUSIC_LYRICS)
    {
      text_center(r, e, &nyabula_font_body_18,
                  scene_text(payload->previous_line, "灯火落进夜里"),
                  -R * 0.30f, color, opacity * 0.22f);
      text_center(r, e, &nyabula_font_title_28,
                  scene_text(payload->current_line, "我听见你"),
                  -R * 0.03f, color, opacity * 0.94f);
      text_center(r, e, &nyabula_font_body_18,
                  scene_text(payload->next_line, "轻轻回应"), R * 0.27f,
                  color, opacity * 0.30f);
    }
  else
    {
      int index;
      text_center(r, e, &nyabula_font_title_20, "频谱", -R * 0.38f, color,
                  opacity * 0.32f);
      for (index = 0; index < 9; index++)
        {
          float wave = 0.5f + 0.5f * sinf(seconds * (3.4f +
                              (index % 3) * 0.24f) + index * 0.88f);
          float envelope = 0.58f + 0.42f *
              sinf((index + 1) / 10.0f * PI);
          float height = R * (0.13f + 0.49f * wave * envelope);
          float x = (index - 4) * R * 0.107f;
          line(r, e, x, -height * 0.5f + R * 0.10f, x,
               height * 0.5f + R * 0.10f, R * 0.062f, color,
               opacity * (0.48f + wave * 0.46f), false);
        }
    }
}

static void scene_timer(struct nyabula_eye_renderer_s *r,
                        const struct eye_s *e,
                        const struct nyabula_eye_scene_payload_s *payload,
                        float seconds, float opacity)
{
  uint32_t color = scene_color(e);
  float total = payload->duration_ms == 0 ? 300.0f
                                          : payload->duration_ms / 1000.0f;
  float remaining = payload->remaining_ms == 0
                        ? fmaxf(0.0f, total - seconds)
                        : payload->remaining_ms / 1000.0f;
  unsigned int rounded = ceilf(remaining);
  char value[12];

  snprintf(value, sizeof(value), "%02u",
           e->id == NYABULA_EYE_LEFT ? rounded / 60 : rounded % 60);
  text_center(r, e, &nyabula_font_english_119, value, -R * 0.03f, color,
              opacity * (remaining <= 10.0f
                              ? 0.82f + sinf(seconds * 8.0f) * 0.18f
                              : 0.96f));
  text_center(r, e, &nyabula_font_body_14,
              e->id == NYABULA_EYE_LEFT ? "分钟" : "秒钟", R * 0.34f,
              color, opacity * 0.42f);
  arc(r, e, 0.0f, 0.0f, R * 0.72f, -PI * 0.5f, PI * 2.0f, R * 0.035f,
      color, opacity * 0.22f);
  arc(r, e, 0.0f, 0.0f, R * 0.72f, -PI * 0.5f,
      clampf(remaining / total, 0.0f, 1.0f) * PI * 2.0f, R * 0.035f,
      color, opacity * 0.82f);
}

static float scene_ease(float value)
{
  float t = clampf(value, 0.0f, 1.0f);
  float u = 1.0f - t;
  return 3.0f * u * u * t * 0.08f + 3.0f * u * t * t * 0.92f +
         t * t * t;
}

static void scene_draw_drop(struct nyabula_eye_renderer_s *r,
                            const struct eye_s *e, float x, float y,
                            float size, float tilt, uint32_t color,
                            float opacity)
{
  struct scene_point_s points[52];
  struct scene_point_s current = {0.0f, -size * 0.72f};
  int count = 0;
  int index;
  float sine = sinf(tilt);
  float cosine = cosf(tilt);

  points[count++] = current;
  scene_append_cubic(points, &count, current,
                     (struct scene_point_s){size * 0.08f, -size * 0.46f},
                     (struct scene_point_s){size * 0.32f, -size * 0.12f},
                     (struct scene_point_s){size * 0.32f, size * 0.10f});
  current = points[count - 1];
  scene_append_cubic(points, &count, current,
                     (struct scene_point_s){size * 0.32f, size * 0.40f},
                     (struct scene_point_s){size * 0.17f, size * 0.58f},
                     (struct scene_point_s){0.0f, size * 0.58f});
  current = points[count - 1];
  scene_append_cubic(points, &count, current,
                     (struct scene_point_s){-size * 0.17f, size * 0.58f},
                     (struct scene_point_s){-size * 0.32f, size * 0.40f},
                     (struct scene_point_s){-size * 0.32f, size * 0.10f});
  current = points[count - 1];
  scene_append_cubic(points, &count, current,
                     (struct scene_point_s){-size * 0.32f, -size * 0.12f},
                     (struct scene_point_s){-size * 0.08f, -size * 0.46f},
                     (struct scene_point_s){0.0f, -size * 0.72f});

  for (index = 0; index < count; index++)
    {
      float px = points[index].x;
      float py = points[index].y;
      points[index].x = x + px * cosine - py * sine;
      points[index].y = y + px * sine + py * cosine;
    }

  scene_fill_polygon(r, e, points, count, color, color, opacity);
}

static void scene_draw_rain(struct nyabula_eye_renderer_s *r,
                            const struct eye_s *e, uint32_t color,
                            float seconds, bool storm, float opacity)
{
  static const float rain_x[] = {-0.34f, -0.18f, -0.02f, 0.15f, 0.31f};
  static const float storm_x[] = {-0.39f, -0.27f, -0.14f, 0.0f,
                                  0.13f, 0.26f, 0.39f};
  const float *positions = storm ? storm_x : rain_x;
  int count = storm ? 7 : 5;
  float drift = sinf(seconds * 0.72f) * R * 0.025f;
  float center_y = -R * 0.09f;
  int index;

  for (index = 0; index < count; index++)
    {
      float phase = fmodf(seconds * ((storm ? 0.78f : 0.48f) +
                                     index * 0.018f) + index * 0.193f,
                          1.0f);
      float y = R * (-0.10f + powf(phase, 1.55f) * 0.86f);
      float fade_in = scene_ease(phase / 0.12f);
      float fade_out = 1.0f - scene_ease((y / R - 0.64f) / 0.08f);
      float alpha = fade_in * fade_out * (0.60f + (index % 2) * 0.22f);

      scene_draw_drop(r, e, R * positions[index] + drift, center_y + y,
                      R * (storm ? 0.042f : 0.048f),
                      storm ? -0.18f : -0.12f, color, opacity * alpha);
      if (y >= R * 0.72f)
        {
          float ripple = clampf((y / R - 0.72f) / 0.04f, 0.0f, 1.0f);
          float radius = R * (0.018f + ripple * 0.08f);
          arc(r, e, R * positions[index] + drift, center_y + R * 0.74f,
              radius, 0.0f, PI * 2.0f, R * 0.012f, color,
              opacity * (1.0f - ripple) * 0.28f);
        }
    }

  if (storm)
    {
      float flash = fmodf(seconds + 1.37f, 6.4f);
      if (flash < 0.30f)
        {
          struct scene_point_s bolt[28];
          struct scene_point_s current = {-R * 0.07f, center_y - R * 0.13f};
          float alpha = sinf(flash / 0.30f * PI);
          int points = 0;

          bolt[points++] = current;
          scene_append_cubic(
              bolt, &points, current,
              (struct scene_point_s){R * 0.10f, center_y + R * 0.04f},
              (struct scene_point_s){-R * 0.10f, center_y + R * 0.20f},
              (struct scene_point_s){R * 0.02f, center_y + R * 0.34f});
          current = bolt[points - 1];
          scene_append_cubic(
              bolt, &points, current,
              (struct scene_point_s){R * 0.15f, center_y + R * 0.47f},
              (struct scene_point_s){-R * 0.02f, center_y + R * 0.57f},
              (struct scene_point_s){R * 0.08f, center_y + R * 0.68f});
          scene_stroke_points(r, e, bolt, points, R * 0.038f,
                              shade(color, 1.42f), opacity * alpha * 0.94f);
        }
    }

  scene_draw_cloud(r, e, color, drift, center_y, storm ? 0.96f : 1.0f,
                   opacity);
}

static void scene_draw_snow(struct nyabula_eye_renderer_s *r,
                            const struct eye_s *e, uint32_t color,
                            float seconds, float opacity)
{
  int index;

  for (index = 0; index < 12; index++)
    {
      float phase = fmodf(seconds * (0.16f + (index % 3) * 0.018f) +
                              index * 0.083f,
                          1.0f);
      float x = R * (-0.46f + fmodf(index * 0.37f, 1.0f) * 0.92f) +
                sinf(seconds * 1.3f + index) * R * 0.035f;
      float y = -R * 0.06f + R * (-0.12f + phase * 0.90f);
      float alpha = scene_ease(phase / 0.12f) *
                    (1.0f - scene_ease((phase - 0.84f) / 0.16f));
      int arm;

      for (arm = 0; arm < 3; arm++)
        {
          float angle = seconds * 0.4f + index + arm * PI / 3.0f;
          float dx = cosf(angle) * R * 0.028f;
          float dy = sinf(angle) * R * 0.028f;
          line(r, e, x - dx, y - dy, x + dx, y + dy, R * 0.012f, color,
               opacity * alpha * 0.78f, false);
        }
    }

  scene_draw_cloud(r, e, color, sinf(seconds * 0.5f) * R * 0.018f,
                   -R * 0.06f, 0.94f, opacity);
}

static void scene_draw_fog(struct nyabula_eye_renderer_s *r,
                           const struct eye_s *e, uint32_t color,
                           float seconds, float opacity)
{
  int index;

  for (index = 0; index < 7; index++)
    {
      float y = R * (-0.50f + index * 0.16f);
      float shift = sinf(seconds * 0.35f + index * 0.9f) * R * 0.10f;
      float half = R * (0.26f + (index % 3) * 0.10f);
      struct scene_point_s points[14];
      int count = 0;

      points[count++] = (struct scene_point_s){-half + shift, y};
      scene_append_cubic(
          points, &count, points[0],
          (struct scene_point_s){-half * 0.35f + shift, y - R * 0.035f},
          (struct scene_point_s){half * 0.35f + shift, y + R * 0.035f},
          (struct scene_point_s){half + shift, y});
      scene_stroke_points(r, e, points, count,
                          R * (0.018f + (index % 2) * 0.008f), color,
                          opacity * (0.22f + index * 0.055f));
    }
}

static void scene_weather(struct nyabula_eye_renderer_s *r,
                          const struct eye_s *e,
                          const struct nyabula_eye_scene_payload_s *payload,
                          float seconds, float opacity)
{
  static const char *names[] = {"晴朗", "多云", "小雨", "雷雨", "降雪", "有雾"};
  static const int temperatures[] = {28, 21, 23, 19, 1, 16};
  static const char *left_labels[] = {"紫外", "湿度", "湿度", "风速", "湿度",
                                      "能见"};
  static const char *left_values[] = {"4", "66%", "78%", "24", "91%", "0.8"};
  static const char *right_labels[] = {"体感", "体感", "体感", "湿度", "体感",
                                       "湿度"};
  static const char *right_values[] = {"29°", "20°", "22°", "86%", "-2°",
                                        "95%"};
  uint32_t color = scene_color(e);
  enum nyabula_eye_weather_e weather = payload->weather;
  char value[20];
  char left_value[16];
  char right_value[16];

  if (e->id == NYABULA_EYE_RIGHT)
    {
      int temperature = payload->temperature_c == 0.0f
                            ? temperatures[weather]
                            : (int)lroundf(payload->temperature_c);
      const char *left_value_text = left_values[weather];
      const char *right_value_text = right_values[weather];

      if (weather == NYABULA_EYE_WEATHER_STORM && payload->wind_kph > 0.0f)
        {
          snprintf(left_value, sizeof(left_value), "%d",
                   (int)lroundf(payload->wind_kph));
          left_value_text = left_value;
        }
      else if (weather == NYABULA_EYE_WEATHER_FOG &&
               payload->visibility_km > 0.0f)
        {
          int tenths = (int)lroundf(payload->visibility_km * 10.0f);
          snprintf(left_value, sizeof(left_value), "%d.%d", tenths / 10,
                   abs(tenths % 10));
          left_value_text = left_value;
        }
      else if (weather != NYABULA_EYE_WEATHER_SUNNY &&
               payload->humidity_percent > 0.0f)
        {
          snprintf(left_value, sizeof(left_value), "%d%%",
                   (int)lroundf(payload->humidity_percent));
          left_value_text = left_value;
        }

      if (weather == NYABULA_EYE_WEATHER_STORM &&
          payload->humidity_percent > 0.0f)
        {
          snprintf(right_value, sizeof(right_value), "%d%%",
                   (int)lroundf(payload->humidity_percent));
          right_value_text = right_value;
        }
      else if (payload->feels_like_c != 0.0f)
        {
          snprintf(right_value, sizeof(right_value), "%d°",
                   (int)lroundf(payload->feels_like_c));
          right_value_text = right_value;
        }

      arc(r, e, 0.0f, 0.0f, R * 0.70f, PI * 0.83f, PI * 1.34f,
          R * 0.024f, color, opacity * 0.16f);
      arc(r, e, 0.0f, 0.0f, R * 0.70f, PI * 0.83f, PI * 0.78f,
          R * 0.024f, color, opacity * 0.76f);
      snprintf(value, sizeof(value), "%d°", temperature);
      text_center(r, e,
                  scene_font(r, FONT_FAMILY_ENGLISH, 85,
                             &nyabula_font_english_96),
                  value, -R * 0.11f, color, opacity * 0.98f);
      text_center(r, e,
                  scene_font(r, FONT_FAMILY_TITLE, 20,
                             &nyabula_font_title_20),
                  names[weather], R * 0.23f, color, opacity * 0.64f);
      text_center_at(r, e,
                     scene_font(r, FONT_FAMILY_BODY, 14,
                                &nyabula_font_body_14),
                     left_labels[weather],
                     -R * 0.23f, R * 0.39f, color, opacity * 0.40f);
      text_center_at(r, e,
                     scene_font(r, FONT_FAMILY_BODY, 14,
                                &nyabula_font_body_14),
                     right_labels[weather],
                     R * 0.23f, R * 0.39f, color, opacity * 0.40f);
      text_center_at(r, e,
                     scene_font(r, FONT_FAMILY_ENGLISH, 18,
                                &nyabula_font_english_18),
                     left_value_text,
                     -R * 0.23f, R * 0.50f, color, opacity * 0.72f);
      text_center_at(r, e,
                     scene_font(r, FONT_FAMILY_ENGLISH, 18,
                                &nyabula_font_english_18),
                     right_value_text,
                     R * 0.23f, R * 0.50f, color, opacity * 0.72f);
      return;
    }

  if (weather == NYABULA_EYE_WEATHER_SUNNY)
    {
      int index;
      for (index = 0; index < 12; index++)
        {
          float angle = index * PI / 6.0f + seconds * 0.06f;
          line(r, e, cosf(angle) * R * 0.48f, sinf(angle) * R * 0.48f,
               cosf(angle) * R * 0.62f, sinf(angle) * R * 0.62f,
               R * 0.026f, color, opacity * 0.42f, false);
        }
      disk(r, e, e->t.cx, e->t.cy, R * 0.36f *
           (1.0f + sinf(seconds * 1.2f) * 0.025f), shade(color, 0.70f),
           opacity, false);
      disk(r, e, e->t.cx - R * 0.08f, e->t.cy - R * 0.10f, R * 0.23f,
           shade(color, 1.28f), opacity * 0.82f, false);
      return;
    }

  if (weather == NYABULA_EYE_WEATHER_FOG)
    {
      scene_draw_fog(r, e, color, seconds, opacity * 0.92f);
      return;
    }

  if (weather == NYABULA_EYE_WEATHER_CLOUDY)
    {
      float drift = sinf(seconds * 0.35f) * R * 0.035f;
      scene_draw_cloud(r, e, shade(color, 0.65f), -R * 0.17f - drift,
                       -R * 0.18f, 0.72f, opacity * 0.34f);
      scene_draw_cloud(r, e, color, R * 0.08f + drift, R * 0.12f, 1.0f,
                       opacity);
    }
  else if (weather == NYABULA_EYE_WEATHER_RAIN ||
           weather == NYABULA_EYE_WEATHER_STORM)
    {
      scene_draw_rain(r, e, color, seconds,
                      weather == NYABULA_EYE_WEATHER_STORM, opacity);
    }
  else if (weather == NYABULA_EYE_WEATHER_SNOW)
    {
      scene_draw_snow(r, e, color, seconds, opacity);
    }
}

static void scene_battery(struct nyabula_eye_renderer_s *r,
                          const struct eye_s *e,
                          const struct nyabula_eye_scene_payload_s *payload,
                          float opacity, float reveal)
{
  static const char *icons[] = {"battery_charging", "battery_low",
                                "battery_full", "battery_hot",
                                "battery_dock"};
  static const char *titles[] = {"充电中", "电量不足", "已充满",
                                 "暂停充电", "正在底座充电"};
  static const char *details[] = {"约 2 小时 14 分", "约 18 分钟",
                                  "可以出发啦", "温度过高",
                                  "磁吸底座已连接"};
  uint32_t color = scene_color(e);
  unsigned int percent = payload->percent;
  char value[8];

  if (percent == 0 && payload->battery_state != NYABULA_EYE_BATTERY_LOW)
    {
      percent = payload->battery_state == NYABULA_EYE_BATTERY_FULL ? 100 : 68;
    }

  if (e->id == NYABULA_EYE_LEFT)
    {
      arc(r, e, 0.0f, 0.0f, R * 0.58f, -PI * 0.5f, PI * 2.0f,
          R * 0.055f, color, opacity * 0.20f);
      arc(r, e, 0.0f, 0.0f, R * 0.58f, -PI * 0.5f,
          percent / 100.0f * PI * 2.0f, R * 0.055f, color,
          opacity * 0.92f);
      icon(r, e, icons[payload->battery_state], 0.78f, color, opacity,
           reveal);
    }
  else
    {
      snprintf(value, sizeof(value), "%u%%", percent);
      text_center(r, e, &nyabula_font_english_96, value, -R * 0.10f, color,
                  opacity * 0.98f);
      text_center(r, e, &nyabula_font_title_20,
                  scene_text(payload->title, titles[payload->battery_state]),
                  R * 0.22f, color, opacity * 0.52f);
      text_center(r, e, &nyabula_font_body_18,
                  scene_text(payload->detail, details[payload->battery_state]),
                  R * 0.42f, color, opacity * 0.72f);
    }
}

static void scene_alarm(struct nyabula_eye_renderer_s *r,
                        const struct eye_s *e,
                        const struct nyabula_eye_scene_payload_s *payload,
                        float seconds, float opacity)
{
  uint32_t color = scene_color(e);
  char value[8];

  if (e->id == NYABULA_EYE_LEFT)
    {
      float shake = sinf(seconds * 14.0f) * R * 0.004f *
                    (0.5f + 0.5f * sinf(seconds * 4.8f));
      int index;
      for (index = 0; index < 3; index++)
        {
          float phase = fmodf(seconds - index * 0.30f, 1.6f);
          if (phase >= 0.0f && phase < 0.8f)
            {
              float p = phase / 0.8f;
              float eased = scene_ease(p);
              arc(r, e, shake, 0.0f, R * (0.34f + eased * 0.34f), 0.0f,
                  PI * 2.0f, R * 0.014f, color,
                  opacity * sinf(p * PI) * 0.28f);
            }
        }
      arc(r, e, shake, R * 0.04f, R * 0.32f, 0.0f, PI * 2.0f,
          R * 0.08f, color, opacity * 0.96f);
      line(r, e, shake, -R * 0.12f, shake, R * 0.04f, R * 0.08f, color,
           opacity, false);
      line(r, e, shake, R * 0.04f, shake + R * 0.08f, R * 0.12f,
           R * 0.08f, color, opacity, false);
      line(r, e, shake - R * 0.28f, -R * 0.36f,
           shake - R * 0.40f, -R * 0.24f, R * 0.08f, color, opacity,
           false);
      line(r, e, shake + R * 0.40f, -R * 0.24f,
           shake + R * 0.28f, -R * 0.36f, R * 0.08f, color, opacity,
           false);
      line(r, e, shake - R * 0.225f, R * 0.308f,
           shake - R * 0.32f, R * 0.40f, R * 0.08f, color, opacity,
           false);
      line(r, e, shake + R * 0.226f, R * 0.307f,
           shake + R * 0.32f, R * 0.40f, R * 0.08f, color, opacity,
           false);
    }
  else
    {
      snprintf(value, sizeof(value), "%02u:%02u",
               payload->hour == 0 ? 7 : payload->hour,
               payload->minute == 0 ? 30 : payload->minute);
      text_center(r, e, &nyabula_font_english_72, value, -R * 0.07f, color,
                  opacity * 0.98f);
      if (payload->alarm_copy != NYABULA_EYE_ALARM_COPY_NONE)
        {
          text_center(
              r, e, &nyabula_font_body_18,
              payload->alarm_copy == NYABULA_EYE_ALARM_COPY_REMINDER
                  ? scene_text(payload->detail, "喝水 · 吃药")
                  : scene_text(payload->title, "早安，Nyabula"),
              R * 0.26f, color, opacity * 0.62f);
        }
    }
}

static void scene_call(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                       const struct nyabula_eye_scene_payload_s *payload,
                       float seconds, float opacity, float reveal)
{
  uint32_t color = scene_color(e);

  if (e->id == NYABULA_EYE_LEFT)
    {
      if (payload->call_state == NYABULA_EYE_CALL_ENDED)
        {
          icon(r, e, "call_end", 1.10f, color, opacity * 0.72f, reveal);
        }
      else
        {
          int index;
          for (index = 0; index < 3; index++)
            {
              float p = fmodf(seconds - index * 0.30f, 1.6f) / 0.8f;
              if (p >= 0.0f && p <= 1.0f)
                {
                  arc(r, e, 0.0f, 0.0f, R * (0.34f + p * 0.34f), 0.0f,
                      PI * 2.0f, R * 0.014f, color,
                      opacity * sinf(p * PI) * 0.28f);
                }
            }
          icon_outline(r, e, "phone_call", 0.912f, R * 0.076f, color,
                       opacity * (0.82f + 0.18f * sinf(seconds * 4.4f)));
        }
    }
  else
    {
      const char *title = payload->call_state == NYABULA_EYE_CALL_ENDED
                              ? "通话结束"
                              : scene_text(payload->title, "BeaconCat");
      const char *detail = payload->call_state == NYABULA_EYE_CALL_ACTIVE
                               ? scene_text(payload->detail, "00:42 · 通话中")
                               : payload->call_state == NYABULA_EYE_CALL_ENDED
                                     ? scene_text(payload->detail,
                                                  "通话时长 03:18")
                                     : scene_text(payload->value,
                                                  "+86 138 0000 0620");
      const lv_font_t *title_font =
          payload->call_state == NYABULA_EYE_CALL_ENDED
              ? &nyabula_font_title_42
              : &nyabula_font_english_42;

      text_center(r, e, title_font, title, -R * 0.11f, color,
                  opacity * (payload->call_state == NYABULA_EYE_CALL_ENDED
                                 ? 0.58f
                                 : 0.98f));
      text_center(r, e, &nyabula_font_body_18, detail, R * 0.17f, color,
                  opacity * 0.48f);
    }
}

static void scene_task(struct nyabula_eye_renderer_s *r, const struct eye_s *e,
                       const struct nyabula_eye_scene_payload_s *payload,
                       float seconds, float opacity, float reveal)
{
  static const float progress[] = {0.62f, 0.14f, 0.46f, 1.0f, 0.68f};
  static const char *values[] = {"3/5", "1/5", "2/5", "5/5", "3/5"};
  static const char *labels[] = {"执行中", "排队中", "等待确认", "已完成",
                                 "执行失败"};
  uint32_t color = scene_color(e);
  float task_progress = payload->progress > 0.0f
                            ? clampf(payload->progress, 0.0f, 1.0f)
                            : progress[payload->task_state];

  if (e->id == NYABULA_EYE_LEFT)
    {
      if (payload->task_state == NYABULA_EYE_TASK_RUNNING)
        {
          const int nodes = 6;
          float rotation = seconds * 0.72f;
          float active = fmodf(seconds * 2.4f, nodes);
          int index;

          for (index = 0; index < nodes; index++)
            {
              float angle = rotation + index * PI * 2.0f / nodes;
              float next = rotation + (index + 1) * PI * 2.0f / nodes;

              line(r, e, cosf(angle) * R * 0.48f,
                   sinf(angle) * R * 0.48f, cosf(next) * R * 0.48f,
                   sinf(next) * R * 0.48f, R * 0.018f, color,
                   opacity * (0.16f + index * 0.025f), false);
            }

          for (index = 0; index < nodes; index++)
            {
              float angle = rotation + index * PI * 2.0f / nodes;
              float distance = fabsf(index - active);
              float alpha;
              float radius;
              float sx;
              float sy;

              distance = fminf(distance, nodes - distance);
              alpha = clampf(0.32f + (1.0f - distance) * 0.55f,
                             0.28f, 0.95f);
              radius = R * (distance < 0.7f ? 0.064f : 0.036f);
              to_screen(&e->t, cosf(angle) * R * 0.48f,
                        sinf(angle) * R * 0.48f, &sx, &sy);
              disk(r, e, sx, sy, radius, color, opacity * alpha, false);
            }

          disk(r, e, e->t.cx, e->t.cy,
               R * 0.082f * (0.90f + 0.10f * sinf(seconds * 4.2f)),
               color, opacity * 0.94f, false);
        }
      else if (payload->task_state == NYABULA_EYE_TASK_QUEUED)
        {
          int index;

          icon_at(r, e, "task_queued", 0.88f, 0.0f, -R * 0.07f,
                  sinf(seconds * 2.1f) * 0.055f, color, opacity, reveal);
          for (index = 0; index < 3; index++)
            {
              bool active = (int)floorf(seconds * 2.2f) % 3 == index;
              float sx;
              float sy;

              to_screen(&e->t, (index - 1) * R * 0.16f, R * 0.48f,
                        &sx, &sy);
              disk(r, e, sx, sy, R * (active ? 0.035f : 0.025f), color,
                   opacity * (active ? 0.92f : 0.24f), false);
            }
        }
      else if (payload->task_state == NYABULA_EYE_TASK_CONFIRM)
        {
          float breathe = 0.96f + 0.04f * sinf(seconds * 2.8f);
          float offset = R * (0.50f + 0.025f * sinf(seconds * 2.8f));

          icon(r, e, "task_confirm", 1.02f * breathe, color, opacity,
               reveal);
          line(r, e, -offset, -R * 0.18f, -offset, R * 0.18f,
               R * 0.022f, color, opacity * 0.46f, false);
          line(r, e, offset, -R * 0.18f, offset, R * 0.18f,
               R * 0.022f, color, opacity * 0.46f, false);
        }
      else if (payload->task_state == NYABULA_EYE_TASK_DONE)
        {
          float phase = clampf(seconds / 0.78f, 0.0f, 1.0f);

          icon(r, e, "task_done", 1.12f, color, opacity, reveal);
          if (phase < 1.0f)
            {
              float eased = scene_ease(phase);
              arc(r, e, 0.0f, 0.0f, R * (0.30f + 0.35f * eased),
                  0.0f, PI * 2.0f, R * 0.022f, color,
                  opacity * (1.0f - eased) * 0.48f);
            }
        }
      else
        {
          float phase = fmodf(seconds, 2.4f);
          float shake = phase < 0.34f
                            ? sinf(phase * 58.0f) * (1.0f - phase / 0.34f) *
                                  R * 0.020f
                            : 0.0f;

          icon_at(r, e, "task_failed", 1.04f, shake, 0.0f, 0.0f,
                  color, opacity * (0.78f + 0.14f * sinf(seconds * 2.6f)),
                  reveal);
          if (phase < 0.62f)
            {
              float p = phase / 0.62f;
              arc(r, e, 0.0f, 0.0f,
                  R * (0.39f + 0.17f * scene_ease(p)), 0.0f, PI * 2.0f,
                  R * 0.018f, color, opacity * (1.0f - p) * 0.28f);
            }
        }
    }
  else
    {
      enum nyabula_eye_task_state_e state = payload->task_state;

      arc(r, e, 0.0f, 0.0f, R * 0.55f, -PI * 0.5f, PI * 2.0f,
          R * 0.05f, color, opacity * 0.18f);
      arc(r, e, 0.0f, 0.0f, R * 0.55f, -PI * 0.5f,
          task_progress * PI * 2.0f, R * 0.05f, color,
          opacity * 0.88f);
      text_center(r, e, &nyabula_font_english_72,
                  scene_text(payload->value, values[state]),
                  -R * 0.04f, color, opacity * 0.98f);
      text_center(r, e, &nyabula_font_title_20,
                  scene_text(payload->title, labels[state]), R * 0.29f,
                  color, opacity * 0.52f);
    }
}

static void scene_draw_content(
    struct nyabula_eye_renderer_s *r, const struct eye_s *e,
    enum nyabula_eye_scene_e scene,
    const struct nyabula_eye_scene_payload_s *payload, float seconds,
    float opacity, float reveal, bool minimal)
{
  uint32_t color = scene_color(e);
  char value[32];
  int index;

  scene_backdrop(r, e, minimal, 0.68f * opacity);
  switch (scene)
    {
      case NYABULA_EYE_SCENE_MUSIC:
        scene_music(r, e, payload, seconds, opacity);
        break;
      case NYABULA_EYE_SCENE_TIMER:
        scene_timer(r, e, payload, seconds, opacity);
        break;
      case NYABULA_EYE_SCENE_WEATHER:
        scene_weather(r, e, payload, seconds, opacity);
        break;
      case NYABULA_EYE_SCENE_BATTERY:
        scene_battery(r, e, payload, opacity, reveal);
        break;
      case NYABULA_EYE_SCENE_ALARM:
        scene_alarm(r, e, payload, seconds, opacity);
        break;
      case NYABULA_EYE_SCENE_CALL:
        scene_call(r, e, payload, seconds, opacity, reveal);
        break;
      case NYABULA_EYE_SCENE_TASK:
        scene_task(r, e, payload, seconds, opacity, reveal);
        break;
      case NYABULA_EYE_SCENE_STOPWATCH:
        {
          unsigned int elapsed = payload->elapsed_ms == 0
                                     ? (unsigned int)(seconds * 1000.0f)
                                     : payload->elapsed_ms;
          unsigned int minutes = elapsed / 60000;
          unsigned int whole_seconds = elapsed / 1000 % 60;
          unsigned int tenths = elapsed / 100 % 10;

          scene_dial(r, e, color, (elapsed % 60000) / 60000.0f, opacity);
          if (e->id == NYABULA_EYE_LEFT)
            {
              snprintf(value, sizeof(value), "%02u", minutes);
              text_center(r, e,
                          scene_font(r, FONT_FAMILY_ENGLISH, 103,
                                     &nyabula_font_english_96),
                          value, -R * 0.04f, color, opacity * 0.98f);
              text_center(r, e,
                          scene_font(r, FONT_FAMILY_BODY, 15,
                                     &nyabula_font_body_14),
                          "分钟", R * 0.34f, color, opacity * 0.44f);
            }
          else
            {
              snprintf(value, sizeof(value), "%02u.%u", whole_seconds,
                       tenths);
              text_center(r, e,
                          scene_font(r, FONT_FAMILY_ENGLISH, 84,
                                     &nyabula_font_english_72),
                          value, -R * 0.04f, color, opacity * 0.98f);
              text_center(r, e,
                          scene_font(r, FONT_FAMILY_BODY, 15,
                                     &nyabula_font_body_14),
                          "秒钟", R * 0.34f, color, opacity * 0.44f);
            }
        }
        break;
      case NYABULA_EYE_SCENE_CALENDAR:
        if (e->id == NYABULA_EYE_LEFT)
          {
            snprintf(value, sizeof(value), "%u", payload->day == 0 ? 17
                                                                    : payload->day);
            rounded_rect_outline(r, e, -R * 0.48f, -R * 0.48f, R * 0.48f,
                                 R * 0.48f, R * 0.10f, R * 0.025f, color,
                                 opacity * 0.24f);
            filled_rect(r, e, -R * 0.48f, -R * 0.29f, R * 0.48f,
                        -R * 0.265f, color, opacity * 0.42f);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_ENGLISH, 89,
                                   &nyabula_font_english_96),
                        value, R * 0.05f, color, opacity * 0.98f);
            snprintf(value, sizeof(value), "%u 月 · 星期日",
                     payload->month == 0 ? 8 : payload->month);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_TITLE, 20,
                                   &nyabula_font_title_20),
                        value, R * 0.36f, color, opacity * 0.52f);
          }
        else
          {
            snprintf(value, sizeof(value), "%02u:%02u",
                     payload->hour == 0 ? 9 : payload->hour,
                     payload->minute == 0 ? 30 : payload->minute);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_ENGLISH, 71,
                                   &nyabula_font_english_72),
                        value, -R * 0.18f, color, opacity * 0.98f);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_TITLE, 25,
                                   &nyabula_font_title_28),
                        scene_text(payload->title, "产品设计评审"),
                        R * 0.13f, color, opacity * 0.67f);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_BODY, 15,
                                   &nyabula_font_body_14),
                        scene_text(payload->detail, "还有 25 分钟"),
                        R * 0.35f, color, opacity * 0.34f);
          }
        break;
      case NYABULA_EYE_SCENE_SLEEP_TIMER:
        if (e->id == NYABULA_EYE_LEFT)
          {
            static const float star_x[] = {-0.46f, 0.40f, 0.47f};
            static const float star_y[] = {-0.34f, -0.35f, 0.25f};
            static const float star_size[] = {0.193f, 0.238f, 0.149f};

            icon_at(r, e, "moon", 1.04f, -R * 0.02f, R * 0.01f, 0.0f,
                    color, opacity, reveal);
            for (index = 0; index < 3; index++)
              {
                float pulse = 0.28f +
                              0.42f * (0.5f + 0.5f *
                                               sinf(seconds * 1.35f +
                                                    index * 1.9f));
                float rotation = sinf(seconds * 0.25f + index) * 0.10f;
                icon_group_at(r, e, "star", star_size[index],
                              R * star_x[index], R * star_y[index], rotation,
                              color, opacity * pulse, reveal);
              }
          }
        else
          {
            unsigned int remaining = payload->remaining_ms == 0
                                         ? 30
                                         : (payload->remaining_ms + 59999) /
                                               60000;
            snprintf(value, sizeof(value), "%u", remaining);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_ENGLISH, 85,
                                   &nyabula_font_english_72),
                        value, -R * 0.12f, color, opacity * 0.98f);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_TITLE, 19,
                                   &nyabula_font_title_20),
                        "分钟", R * 0.18f, color, opacity * 0.55f);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_BODY, 14,
                                   &nyabula_font_body_14),
                        "音乐结束后休眠", R * 0.36f, color, opacity * 0.30f);
          }
        break;
      case NYABULA_EYE_SCENE_NETWORK:
        if (e->id == NYABULA_EYE_LEFT)
          {
            const char *name = payload->network_state == NYABULA_EYE_NETWORK_WIFI
                                   ? "wifi"
                                   : payload->network_state ==
                                             NYABULA_EYE_NETWORK_BLUETOOTH
                                         ? "bluetooth"
                                         : "wifi_off";
            icon(r, e, name, 1.18f, color,
                 opacity * (payload->network_state ==
                                        NYABULA_EYE_NETWORK_OFFLINE
                                    ? 0.72f
                                    : 1.0f),
                 reveal);
          }
        else
          {
            const char *title = payload->network_state == NYABULA_EYE_NETWORK_WIFI
                                    ? "网络已连接"
                                    : payload->network_state ==
                                              NYABULA_EYE_NETWORK_BLUETOOTH
                                          ? "蓝牙已连接"
                                          : "当前离线";
            const char *detail =
                payload->network_state == NYABULA_EYE_NETWORK_WIFI
                    ? "Nyabula · 5 GHz"
                    : payload->network_state == NYABULA_EYE_NETWORK_BLUETOOTH
                          ? "BeaconPhone"
                          : "等待重新连接";
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_TITLE, 39,
                                   &nyabula_font_title_42),
                        title, -R * 0.13f, color,
                        opacity * (payload->network_state ==
                                           NYABULA_EYE_NETWORK_OFFLINE
                                       ? 0.54f
                                       : 0.98f));
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_BODY, 16,
                                   &nyabula_font_body_18),
                        detail, R * 0.16f, color, opacity * 0.44f);
            if (payload->network_state != NYABULA_EYE_NETWORK_OFFLINE)
              {
                float sx;
                float sy;
                float pulse = 0.55f + 0.35f * sinf(seconds * 2.2f);
                to_screen(&e->t, 0.0f, R * 0.38f, &sx, &sy);
                disk(r, e, sx, sy, R * 0.025f, color, opacity * pulse,
                     false);
              }
          }
        break;
      case NYABULA_EYE_SCENE_AUDIO:
        if (e->id == NYABULA_EYE_LEFT)
          {
            static const char *names[] = {"speaker", "headphones",
                                          "audio_both", "audio_mute"};
            scene_dial(r, e, color,
                       payload->audio_route == NYABULA_EYE_AUDIO_MUTE
                           ? 0.0f
                           : 0.64f,
                       opacity);
            icon(r, e, names[payload->audio_route], 0.82f, color,
                 opacity * (payload->audio_route == NYABULA_EYE_AUDIO_MUTE
                                ? 0.64f
                                : 1.0f),
                 reveal);
          }
        else
          {
            static const char *titles[] = {"扬声器", "耳机", "同时输出",
                                           "已静音"};
            bool muted = payload->audio_route == NYABULA_EYE_AUDIO_MUTE;
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_TITLE, 39,
                                   &nyabula_font_title_42),
                        titles[payload->audio_route], -R * 0.14f, color,
                        opacity * (muted ? 0.48f : 0.98f));
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_ENGLISH, 64,
                                   &nyabula_font_english_72),
                        muted ? "—" : "64%", R * 0.14f, color,
                        opacity * 0.74f);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_BODY, 15,
                                   &nyabula_font_body_14),
                        "媒体音量", R * 0.39f, color, opacity * 0.34f);
          }
        break;
      case NYABULA_EYE_SCENE_EQ:
        if (e->id == NYABULA_EYE_LEFT)
          {
            bool calibrating =
                payload->eq_view == NYABULA_EYE_EQ_CALIBRATING;
            if (calibrating)
              {
                float progress = fmodf(seconds * 0.13f, 1.0f);
                unsigned int frequency =
                    (unsigned int)roundf(20.0f * powf(1000.0f, progress));
                scene_dial(r, e, color, progress, opacity);
                snprintf(value, sizeof(value), "%u", frequency);
                text_center(r, e,
                            scene_font(r, FONT_FAMILY_ENGLISH, 59,
                                       &nyabula_font_english_72),
                            value, -R * 0.04f, color, opacity * 0.98f);
                text_center(r, e,
                            scene_font(r, FONT_FAMILY_BODY, 15,
                                       &nyabula_font_body_14),
                            "Hz · 扫频", R * 0.28f, color, opacity * 0.42f);
              }
            else
              {
                bool has_bands = false;
                for (index = 0; index < NYABULA_EYE_EQ_BANDS; index++)
                  {
                    if (fabsf(payload->eq_bands[index]) > 0.001f)
                      {
                        has_bands = true;
                        break;
                      }
                  }

                for (index = 0; index < 7; index++)
                  {
                    float x = (index - 3) * R * 0.16f;
                    float level = has_bands
                                      ? clampf(fabsf(payload->eq_bands[index]) /
                                                   12.0f,
                                               0.0f, 1.0f)
                                      : 0.5f +
                                            0.5f * sinf(index * 1.6f +
                                                        seconds * 0.7f);
                    float height = R * (0.22f + 0.28f * level);
                    line(r, e, x, -height * 0.5f, x, height * 0.5f,
                         R * 0.07f, color,
                         opacity * (0.30f + index * 0.07f), false);
                  }
              }
          }
        else
          {
            bool calibrating =
                payload->eq_view == NYABULA_EYE_EQ_CALIBRATING;
            struct eye_s shifted = *e;
            shifted.t.cy -= R * 0.08f;
            scene_eq_curve(r, &shifted, color, seconds, opacity,
                           calibrating);
            text_center(r, e,
                        scene_font(r,
                                   calibrating ? FONT_FAMILY_TITLE
                                               : FONT_FAMILY_ENGLISH,
                                   calibrating ? 18 : 15,
                                   calibrating ? &nyabula_font_title_20
                                               : &nyabula_font_english_18),
                        calibrating ? "测量中 · 2/4" : "NYABULA FLAT",
                        R * 0.43f, color, opacity * 0.58f);
          }
        break;
      case NYABULA_EYE_SCENE_CAPTION:
        if (e->id == NYABULA_EYE_LEFT)
          {
            float last_x = -R * 0.58f;
            float last_y = 0.0f;
            for (index = 1; index <= 60; index++)
              {
                float x = R * (-0.58f + index / 60.0f * 1.16f);
                float envelope = sinf(index / 60.0f * PI);
                float y = sinf(index * 0.72f + seconds * 5.0f) * envelope *
                          R * 0.25f;
                line(r, e, last_x, last_y, x, y, R * 0.025f, color,
                     opacity * 0.82f, false);
                last_x = x;
                last_y = y;
              }
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_TITLE, 16,
                                   &nyabula_font_title_20),
                        "聆听中", R * 0.42f, color, opacity * 0.40f);
          }
        else
          {
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_BODY, 17,
                                   &nyabula_font_body_18),
                        scene_text(payload->previous_line, "我可以帮你"),
                        -R * 0.27f, color, opacity * 0.28f);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_TITLE, 32,
                                   &nyabula_font_title_28),
                        scene_text(payload->current_line, "设置一个提醒"),
                        0.0f, color, opacity * 0.98f);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_BODY, 16,
                                   &nyabula_font_body_18),
                        scene_text(payload->next_line, "字幕模式已开启"),
                        R * 0.29f, color, opacity * 0.34f);
          }
        break;
      case NYABULA_EYE_SCENE_BRIEFING:
        if (e->id == NYABULA_EYE_LEFT)
          {
            float progress = fmodf(seconds * 0.16f, 1.0f);
            float last_x = -R * 0.32f;
            float last_y = 0.0f;

            arc(r, e, 0.0f, 0.0f, R * 0.48f, 0.0f, PI * 2.0f,
                R * 0.018f, color, opacity * 0.18f);
            for (index = 0; index < 3; index++)
              {
                float angle = -PI * 0.5f +
                              (index + progress) * PI * 2.0f / 3.0f;
                float sx;
                float sy;
                to_screen(&e->t, cosf(angle) * R * 0.48f,
                          sinf(angle) * R * 0.48f, &sx, &sy);
                disk(r, e, sx, sy,
                     R * (index == 1 ? 0.065f : 0.035f), color,
                     opacity * (index == 1 ? 0.92f : 0.40f), false);
              }

            for (index = 1; index <= 40; index++)
              {
                float x = R * (-0.32f + index / 40.0f * 0.64f);
                float y = sinf(index * 0.66f + seconds * 4.0f) * R * 0.09f *
                          (1.0f - fabsf(index / 20.0f - 1.0f));
                line(r, e, last_x, last_y, x, y, R * 0.026f, color,
                     opacity * 0.78f, false);
                last_x = x;
                last_y = y;
              }
          }
        else
          {
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_TITLE, 39,
                                   &nyabula_font_title_42),
                        scene_text(payload->title, "早间简报"),
                        -R * 0.24f, color, opacity * 0.98f);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_BODY, 18,
                                   &nyabula_font_body_18),
                        scene_text(payload->subtitle,
                                   "天气 · 日程 · 资讯"),
                        R * 0.03f, color,
                        opacity * 0.55f);
            snprintf(value, sizeof(value), "正在朗读  %u / %u",
                     payload->briefing_index == 0
                         ? 1
                         : payload->briefing_index,
                     payload->briefing_count == 0
                         ? 3
                         : payload->briefing_count);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_BODY, 15,
                                   &nyabula_font_body_14),
                        scene_text(payload->detail, value), R * 0.29f, color,
                        opacity * 0.36f);
          }
        break;
      case NYABULA_EYE_SCENE_PRIVACY:
      case NYABULA_EYE_SCENE_IDENTITY:
      case NYABULA_EYE_SCENE_MEMORY:
      case NYABULA_EYE_SCENE_DEVICES:
      case NYABULA_EYE_SCENE_SYSTEM:
      case NYABULA_EYE_SCENE_HEALTH:
      case NYABULA_EYE_SCENE_PRESENCE:
      case NYABULA_EYE_SCENE_COMPANION:
      case NYABULA_EYE_SCENE_SUBWOOFER:
        if (e->id == NYABULA_EYE_LEFT)
          {
            if (scene == NYABULA_EYE_SCENE_COMPANION)
              {
                for (index = 3; index >= 0; index--)
                  {
                    float phase = fmodf(seconds, 6.8f) - index * 0.22f;
                    float breath = phase >= 0.0f && phase < 1.05f
                                       ? phase / 1.05f
                                       : phase < 1.30f ? 1.0f
                                       : phase < 2.15f
                                             ? 1.0f - (phase - 1.30f) / 0.85f
                                             : 0.0f;
                    float radius = R * (0.22f + index * 0.11f) *
                                   (0.90f + breath * 0.24f);
                    disk(r, e, e->t.cx, e->t.cy, radius, color,
                         opacity * (0.05f + index * 0.032f +
                                    breath * 0.026f),
                         false);
                  }
                icon(r, e, "companion", 0.48f, color, opacity * 0.94f,
                     reveal);
              }
            else
              {
                const char *name =
                    scene == NYABULA_EYE_SCENE_PRIVACY ? "privacy"
                    : scene == NYABULA_EYE_SCENE_IDENTITY ? "identity"
                    : scene == NYABULA_EYE_SCENE_MEMORY   ? "memory"
                    : scene == NYABULA_EYE_SCENE_DEVICES  ? "devices"
                    : scene == NYABULA_EYE_SCENE_SYSTEM   ? "system"
                    : scene == NYABULA_EYE_SCENE_HEALTH   ? "heart_rate"
                    : scene == NYABULA_EYE_SCENE_PRESENCE ? "presence"
                                                          : "subwoofer";
                float pulse = scene == NYABULA_EYE_SCENE_PRIVACY
                                  ? 0.88f + 0.12f * sinf(seconds * 2.4f)
                              : scene == NYABULA_EYE_SCENE_MEMORY
                                  ? 0.90f + 0.10f * sinf(seconds * 1.7f)
                              : scene == NYABULA_EYE_SCENE_HEALTH
                                  ? 0.90f + 0.10f * sinf(seconds * 4.0f)
                              : scene == NYABULA_EYE_SCENE_SUBWOOFER
                                  ? 0.84f +
                                        0.16f * (0.5f +
                                                 0.5f * sinf(seconds * 4.4f))
                                  : 1.0f;
                float size = scene == NYABULA_EYE_SCENE_PRIVACY ||
                                     scene == NYABULA_EYE_SCENE_MEMORY
                                 ? 1.16f
                                 : 1.18f;
                if (scene == NYABULA_EYE_SCENE_SUBWOOFER)
                  {
                    size += (0.5f + 0.5f * sinf(seconds * 4.4f)) * 0.025f;
                  }

                icon(r, e, name, size, color, opacity * pulse, reveal);
              }
          }
        else
          {
            const char *title;
            const char *subtitle;
            const char *detail;
            enum font_family_e title_family = FONT_FAMILY_TITLE;
            uint16_t title_size;
            uint16_t subtitle_size;
            uint16_t detail_size;
            float title_y;
            float subtitle_y;
            float detail_y;
            float title_alpha = 0.98f;
            float subtitle_alpha;
            float detail_alpha;

            switch (scene)
              {
                case NYABULA_EYE_SCENE_PRIVACY:
                  title = payload->privacy_camera &&
                                  payload->privacy_microphone
                              ? "摄像头 + 麦克风"
                          : payload->privacy_camera ? "摄像头"
                          : payload->privacy_microphone ? "麦克风"
                                                        : "隐私设备";
                  subtitle = payload->active ? "正在使用" : "当前空闲";
                  detail = "本地处理 · 指示不可关闭";
                  title_size = 30;
                  subtitle_size = 25;
                  detail_size = 14;
                  title_y = -R * 0.20f;
                  subtitle_y = R * 0.06f;
                  detail_y = R * 0.31f;
                  subtitle_alpha = 0.72f;
                  detail_alpha = 0.34f;
                  break;
                case NYABULA_EYE_SCENE_IDENTITY:
                  title = "你好，主人";
                  subtitle = "主人身份已确认";
                  detail = "模板仅保存在本机";
                  title_size = 41;
                  subtitle_size = 19;
                  detail_size = 13;
                  title_y = -R * 0.15f;
                  subtitle_y = R * 0.14f;
                  detail_y = R * 0.34f;
                  subtitle_alpha = 0.58f;
                  detail_alpha = 0.30f;
                  break;
                case NYABULA_EYE_SCENE_MEMORY:
                  title = "你喜欢轻音乐";
                  subtitle = "准备记住";
                  detail = "等待语音确认";
                  title_size = 30;
                  subtitle_size = 16;
                  detail_size = 15;
                  title_y = -R * 0.04f;
                  subtitle_y = -R * 0.31f;
                  detail_y = R * 0.26f;
                  subtitle_alpha = 0.55f;
                  detail_alpha = 0.42f;
                  break;
                case NYABULA_EYE_SCENE_DEVICES:
                  snprintf(value, sizeof(value), "%u",
                           payload->device_count == 0
                               ? 2
                               : payload->device_count);
                  title = value;
                  subtitle = "台设备在线";
                  detail = "手机 · CODEX · 局域网 MCP";
                  title_family = FONT_FAMILY_ENGLISH;
                  title_size = 66;
                  subtitle_size = 20;
                  detail_size = 13;
                  title_y = -R * 0.18f;
                  subtitle_y = R * 0.08f;
                  detail_y = R * 0.31f;
                  subtitle_alpha = 0.58f;
                  detail_alpha = 0.32f;
                  break;
                case NYABULA_EYE_SCENE_SYSTEM:
                  title = "OPENVELA";
                  subtitle = "本地大脑就绪";
                  detail = "AMP · NPU 可用";
                  title_family = FONT_FAMILY_ENGLISH;
                  title_size = 39;
                  subtitle_size = 19;
                  detail_size = 14;
                  title_y = -R * 0.24f;
                  subtitle_y = R * 0.01f;
                  detail_y = R * 0.27f;
                  subtitle_alpha = 0.58f;
                  detail_alpha = 0.31f;
                  break;
                case NYABULA_EYE_SCENE_HEALTH:
                  snprintf(value, sizeof(value), "%d",
                           (int)lroundf(payload->heart_rate_bpm <= 0.0f
                                            ? 72.0f
                                            : payload->heart_rate_bpm));
                  title = value;
                  subtitle = payload->signal_good ? "次/分 · 信号良好"
                                                  : "次/分 · 信号较弱";
                  detail = "趋势参考 · 非医疗用途";
                  title_family = FONT_FAMILY_ENGLISH;
                  title_size = 85;
                  subtitle_size = 18;
                  detail_size = 12;
                  title_y = -R * 0.13f;
                  subtitle_y = R * 0.16f;
                  detail_y = R * 0.37f;
                  subtitle_alpha = 0.56f;
                  detail_alpha = 0.28f;
                  break;
                case NYABULA_EYE_SCENE_PRESENCE:
                  title = "用户在场";
                  {
                    float distance = payload->distance_m <= 0.0f
                                         ? 0.8f
                                         : payload->distance_m;
                    int tenths = (int)lroundf(distance * 10.0f);
                    snprintf(value, sizeof(value), "%d.%d m", tenths / 10,
                             abs(tenths % 10));
                  }
                  subtitle = value;
                  detail = "正在看向猫猫";
                  title_size = 39;
                  subtitle_size = 55;
                  detail_size = 13;
                  title_y = -R * 0.20f;
                  subtitle_y = R * 0.09f;
                  detail_y = R * 0.34f;
                  subtitle_alpha = 0.68f;
                  detail_alpha = 0.30f;
                  break;
                case NYABULA_EYE_SCENE_COMPANION:
                  title = "专注陪伴";
                  subtitle = "安静模式";
                  detail = "仅保留重要提醒";
                  title_size = 39;
                  subtitle_size = 16;
                  detail_size = 13;
                  title_y = -R * 0.17f;
                  subtitle_y = R * 0.10f;
                  detail_y = R * 0.32f;
                  subtitle_alpha = 0.50f;
                  detail_alpha = 0.27f;
                  break;
                default:
                  title = "2.1 已启用";
                  snprintf(value, sizeof(value), "%d Hz",
                           (int)lroundf(payload->crossover_hz <= 0.0f
                                            ? 80.0f
                                            : payload->crossover_hz));
                  subtitle = value;
                  detail = "低音在线 · LR 分频";
                  title_size = 48;
                  subtitle_size = 59;
                  detail_size = 13;
                  title_y = -R * 0.22f;
                  subtitle_y = R * 0.08f;
                  detail_y = R * 0.34f;
                  subtitle_alpha = 0.70f;
                  detail_alpha = 0.31f;
                  break;
              }

            text_center(r, e,
                        scene_font(r, title_family, title_size,
                                   title_family == FONT_FAMILY_ENGLISH
                                       ? &nyabula_font_english_42
                                       : &nyabula_font_title_42),
                        scene_text(payload->title, title), title_y, color,
                        opacity * title_alpha);
            text_center(r, e,
                        scene_font(r,
                                   scene == NYABULA_EYE_SCENE_PRESENCE ||
                                           scene ==
                                               NYABULA_EYE_SCENE_SUBWOOFER
                                       ? FONT_FAMILY_BODY
                                       : FONT_FAMILY_TITLE,
                                   subtitle_size, &nyabula_font_title_20),
                        scene_text(payload->subtitle, subtitle), subtitle_y,
                        color, opacity * subtitle_alpha);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_BODY, detail_size,
                                   &nyabula_font_body_14),
                        scene_text(payload->detail, detail), detail_y, color,
                        opacity * detail_alpha);
          }
        break;
      case NYABULA_EYE_SCENE_HOME:
        if (e->id == NYABULA_EYE_LEFT)
          {
            line(r, e, -R * 0.48f, R * 0.12f, 0.0f, -R * 0.37f,
                 R * 0.026f, color, opacity * 0.78f, false);
            line(r, e, 0.0f, -R * 0.37f, R * 0.48f, R * 0.12f,
                 R * 0.026f, color, opacity * 0.78f, false);
            line(r, e, R * 0.48f, R * 0.12f, R * 0.37f, R * 0.12f,
                 R * 0.026f, color, opacity * 0.78f, false);
            line(r, e, R * 0.37f, R * 0.12f, R * 0.37f, R * 0.42f,
                 R * 0.026f, color, opacity * 0.78f, false);
            line(r, e, R * 0.37f, R * 0.42f, -R * 0.37f, R * 0.42f,
                 R * 0.026f, color, opacity * 0.78f, false);
            line(r, e, -R * 0.37f, R * 0.42f, -R * 0.37f, R * 0.12f,
                 R * 0.026f, color, opacity * 0.78f, false);
            line(r, e, -R * 0.37f, R * 0.12f, -R * 0.48f, R * 0.12f,
                 R * 0.026f, color, opacity * 0.78f, false);
            arc(r, e, 0.0f, R * 0.20f, R * 0.14f, PI, PI,
                R * 0.026f, color, opacity * 0.78f);
            line(r, e, R * 0.14f, R * 0.20f, R * 0.14f, R * 0.42f,
                 R * 0.026f, color, opacity * 0.78f, false);
            line(r, e, -R * 0.14f, R * 0.42f, -R * 0.14f, R * 0.20f,
                 R * 0.026f, color, opacity * 0.78f, false);
            {
              float sx;
              float sy;
              to_screen(&e->t, R * 0.34f, -R * 0.30f, &sx, &sy);
              disk(r, e, sx, sy, R * 0.055f, color,
                   opacity * (0.38f + 0.22f * sinf(seconds * 0.8f)), false);
            }
          }
        else
          {
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_TITLE, 39,
                                   &nyabula_font_title_42),
                        "虚拟猫舍", -R * 0.23f, color, opacity * 0.98f);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_BODY, 20,
                                   &nyabula_font_body_18),
                        "晴 · 午后", R * 0.03f, color, opacity * 0.58f);
            text_center(r, e,
                        scene_font(r, FONT_FAMILY_BODY, 14,
                                   &nyabula_font_body_14),
                        "双屏立体视窗预览", R * 0.29f, color,
                        opacity * 0.30f);
          }
        break;
      default:
        break;
    }
}

static void overlays(struct nyabula_eye_renderer_s *r, const struct eye_s *e)
{
  float alpha = e->f->overlay;
  float time = e->f->global_seconds;
  uint32_t bright = shade(e->f->iris_rgb, 1.9f);
  if (alpha <= 0.02f)
    {
      return;
    }

  if (e->f->expression == NYABULA_EYE_EXPRESSION_PROCESSING)
    {
      float rotation = time * 2.6f;
      int i;
      for (i = 0; i < 14; i++)
        {
          float fade = 1.0f - (float)i / 14.0f;
          arc(r, e, e->gx, e->gy, e->ir * 0.46f, rotation - i * 0.16f - 0.17f,
              0.19f, e->ir * 0.055f * (1.0f - i / 14.0f * 0.6f), bright,
              powf(fade, 1.6f) * 0.95f * alpha);
        }

      {
        float sx;
        float sy;
        float breath = 0.7f + 0.3f * sinf(time * 4.0f);
        to_screen(&e->t, e->gx + cosf(rotation) * e->ir * 0.46f,
                  e->gy + sinf(rotation) * e->ir * 0.46f, &sx, &sy);
        disk(r, e, sx, sy, e->ir * 0.05f, 0xffffff, alpha, false);
        to_screen(&e->t, e->gx, e->gy, &sx, &sy);
        disk(r, e, sx, sy, e->ir * 0.055f * (1.0f + breath * 0.5f), bright,
             alpha * (0.55f + breath * 0.4f), false);
      }

      arc(r, e, e->gx, e->gy, e->ir * 0.62f, -rotation * 0.5f, 2.4f,
          e->ir * 0.02f, bright, alpha * 0.35f);
      {
        float duration = 1.0f / 0.9f;
        float rt = fmodf(e->f->mode_seconds, duration + 0.2f);
        if (rt < duration)
          {
            float p = rt / duration;
            arc(r, e, e->gx, e->gy, e->ir * (0.08f + p * 0.3f), 0.0f,
                PI * 2.0f, e->ir * 0.02f, bright, alpha * (1.0f - p) * 0.5f);
          }
      }
    }
  else if (e->f->expression == NYABULA_EYE_EXPRESSION_STAR)
    {
      float pulse = 1.0f + sinf(time * 5.0f) * 0.12f;
      star(r, e, e->gx, e->gy, e->ir * 0.5f * pulse,
           sinf(time * 1.5f) * 0.25f - PI * 0.5f, 0xffd94d, alpha);
      {
        int i;
        for (i = 0; i < 2; i++)
          {
            float a = time * 2.0f + i * 3.0f +
                      (e->id == NYABULA_EYE_LEFT ? -1.0f : 1.0f);
            star(r, e, e->gx + cosf(a) * e->ir * 0.75f,
                 e->gy + sinf(a) * e->ir * 0.60f, e->ir * 0.09f, a, 0xfff3b8,
                 alpha * (0.4f + 0.3f * sinf(time * 4.0f + i * 2.0f)));
          }
      }
    }
  else if (e->f->expression == NYABULA_EYE_EXPRESSION_HEART)
    {
      float beat = fmodf(time * 1.6f, 1.0f);
      float pulse =
          1.0f + (beat < 0.15f ? sinf(beat / 0.15f * PI) * 0.18f : 0.0f);
      heart(r, e, e->gx, e->gy, e->ir * 0.5f * pulse, 0xff4d79, alpha);
    }
  else if (e->f->expression == NYABULA_EYE_EXPRESSION_DIZZY)
    {
      float rotation = time * 4.0f * (e->id == NYABULA_EYE_LEFT ? 1 : -1);
      float a;
      float lx = e->gx;
      float ly = e->gy;
      for (a = 0.15f; a < PI * 5.0f; a += 0.15f)
        {
          float radius = e->ir * 0.55f * a / (PI * 5.0f);
          float x = e->gx + cosf(a + rotation) * radius;
          float y = e->gy + sinf(a + rotation) * radius;
          line(r, e, lx, ly, x, y, e->ir * 0.09f, 0x0a0c10, alpha, false);
          lx = x;
          ly = y;
        }
    }
  else if (e->f->expression == NYABULA_EYE_EXPRESSION_SAD)
    {
      int x;
      int y;
      float shimmer = alpha * (0.5f + 0.2f * sinf(time * 2.0f));
      for (y = e->t.cy - R; y <= e->t.cy + R; y++)
        {
          for (x = e->t.cx - R; x <= e->t.cx + R; x++)
            {
              float lx;
              float ly;
              float dx;
              float dy;
              if (!in_screen(e, x + 0.5f, y + 0.5f))
                {
                  continue;
                }

              to_local(&e->t, x + 0.5f, y + 0.5f, &lx, &ly);
              dx = lx - e->gx;
              dy = ly - e->gy;
              if (hypotf(dx, dy) <= e->ir * 0.98f && dy > e->ir * 0.3f)
                {
                  pixel(r, x, y, 0xbee1ff,
                        shimmer * 0.55f *
                            clampf((dy - e->ir * 0.3f) / (e->ir * 0.7f), 0.0f,
                                   1.0f));
                }
            }
        }
    }
}

static void highlights(struct nyabula_eye_renderer_s *r, const struct eye_s *e)
{
  ellipse(r, e, -e->ir * 0.33f + e->gx * 0.55f, -e->ir * 0.38f + e->gy * 0.55f,
          e->ir * 0.13f, e->ir * 0.10f, -0.5f, 0xffffff, 0.9f);
  ellipse(r, e, e->ir * 0.30f + e->gx * 0.6f, e->ir * 0.24f + e->gy * 0.6f,
          e->ir * 0.05f, e->ir * 0.05f, 0.0f, 0xffffff, 0.45f);
}

static void lids(struct nyabula_eye_renderer_s *r, const struct eye_s *e)
{
  int x;
  int y;
  for (y = e->t.cy - R; y <= e->t.cy + R; y++)
    {
      for (x = e->t.cx - R; x <= e->t.cx + R; x++)
        {
          if (in_screen(e, x + 0.5f, y + 0.5f) &&
              in_lids(e, x + 0.5f, y + 0.5f))
            {
              pixel(r, x, y, 0x000000, 1.0f);
            }
        }
    }
}

static float randomf(struct nyabula_eye_renderer_s *r)
{
  r->random = r->random * 1664525u + 1013904223u;
  return (float)((r->random >> 8) & 0xffffffu) / 16777215.0f;
}

static void update_z(struct nyabula_eye_renderer_s *r,
                     const struct nyabula_eye_frame_s *f)
{
  float dt = r->last_time == 0.0f ? 0.0f : f->global_seconds - r->last_time;
  int i;
  r->last_time = f->global_seconds;
  dt = clampf(dt, 0.0f, 0.05f);
  if (f->expression == NYABULA_EYE_EXPRESSION_SLEEP &&
      f->lid_top + f->lid_bottom > 0.92f)
    {
      r->znext -= dt;
      if (r->znext <= 0.0f)
        {
          r->znext = 0.8f + randomf(r) * 1.2f;
          for (i = 0; i < 2; i++)
            {
              int n;
              for (n = 0; n < ZCOUNT; n++)
                {
                  if (!r->z[n].active)
                    {
                      struct z_s *z = &r->z[n];
                      z->eye = i;
                      z->ox = (randomf(r) * 2.0f - 1.0f) * R * 0.55f;
                      z->y = R * 1.1f;
                      z->vy = -(R * 0.30f + randomf(r) * R * 0.15f);
                      z->sway = R * (0.08f + randomf(r) * 0.10f);
                      z->phase = randomf(r) * 7.0f;
                      z->size = R * (0.15f + randomf(r) * 0.15f);
                      z->active = true;
                      break;
                    }
                }
            }
        }
    }

  for (i = 0; i < ZCOUNT; i++)
    {
      if (r->z[i].active)
        {
          r->z[i].life += dt;
          r->z[i].y += r->z[i].vy * dt;
          if (r->z[i].y < -R * 1.2f ||
              f->expression != NYABULA_EYE_EXPRESSION_SLEEP)
            {
              r->z[i].active = false;
            }
        }
    }
}

static void draw_z(struct nyabula_eye_renderer_s *r, const struct eye_s *e)
{
  int i;
  for (i = 0; i < ZCOUNT; i++)
    {
      struct z_s *z = &r->z[i];
      if (z->active && z->eye == e->id)
        {
          float p = clampf((R * 1.1f - z->y) / (R * 2.2f), 0.0f, 1.0f);
          float a = sinf(p * PI) * 0.85f;
          float x = z->ox + sinf(z->life * 2.0f + z->phase) * z->sway;
          float s = z->size * (1.0f + p * 0.5f);
          line(r, e, x - s * 0.3f, z->y - s * 0.4f, x + s * 0.3f,
               z->y - s * 0.4f, s * 0.11f, 0xa5c3ff, a, true);
          line(r, e, x + s * 0.3f, z->y - s * 0.4f, x - s * 0.3f,
               z->y + s * 0.4f, s * 0.11f, 0xa5c3ff, a, true);
          line(r, e, x - s * 0.3f, z->y + s * 0.4f, x + s * 0.3f,
               z->y + s * 0.4f, s * 0.11f, 0xa5c3ff, a, true);
        }
    }
}

static void prepare(struct eye_s *e, const struct nyabula_eye_frame_s *f,
                    int id)
{
  float side = id == NYABULA_EYE_LEFT ? -1.0f : 1.0f;
  float rotation = f->expression == NYABULA_EYE_EXPRESSION_DIZZY
                       ? sinf(f->global_seconds * 3.0f + side) * 0.06f
                       : 0.0f;
  float extent;
  float top;
  float left;
  float right;
  float top_bulge;
  float bottom;
  float bottom_bulge;
  int index;
  memset(e, 0, sizeof(*e));
  e->f = f;
  e->id = id;
  e->t.cx = W * 0.5f + side * GAP * 0.5f;
  e->t.cy = CY;
  e->t.sy = 1.0f - f->squint * 0.25f;
  e->t.sn = sinf(rotation);
  e->t.cs = cosf(rotation);
  e->ir = R * f->iris_scale;
  e->gx = f->gaze_x * R * 0.30f + side * f->derp * R * 0.24f;
  e->gy =
      f->gaze_y * R * 0.26f + side * f->derp * R * 0.11f + f->derp * R * 0.06f;
  e->lt = f->lid_top;
  e->lb = f->lid_bottom;
  e->slant = f->lid_slant * side * -1.0f;
  e->curve = f->bottom_curve;
  extent = e->ir * 1.04f;
  top = -extent + e->lt * extent * 2.0f;
  left = top - e->slant * extent * 0.35f;
  right = top + e->slant * extent * 0.35f;
  top_bulge = extent * 0.16f * clampf(e->lt * 2.0f, 0.0f, 1.0f);
  bottom = extent - e->lb * extent * 2.0f;
  bottom_bulge = -e->curve * extent * 0.55f -
                 extent * 0.16f * clampf(e->lb * 2.0f, 0.0f, 1.0f);
  for (index = 0; index < LID_SAMPLES; index++)
    {
      float x = index - LID_OFFSET;
      e->top_y[index] = lid_y(x, extent, left, right, top_bulge, true);
      e->bottom_y[index] =
          lid_y(x, extent, bottom, bottom, bottom_bulge, false);
    }
}

static void prepare_scene(struct eye_s *e,
                          const struct nyabula_eye_frame_s *frame, int id)
{
  float side = id == NYABULA_EYE_LEFT ? -1.0f : 1.0f;

  memset(e, 0, sizeof(*e));
  e->f = frame;
  e->id = id;
  e->t.cx = W * 0.5f + side * GAP * 0.5f;
  e->t.cy = CY;
  e->t.sy = 1.0f;
  e->t.cs = 1.0f;
  e->ir = R * 1.08f;
}

static void prepare_scene_lids(struct eye_s *e,
                               const struct nyabula_eye_frame_s *frame,
                               int id, float lid)
{
  struct nyabula_eye_frame_s mask = *frame;

  mask.lid_top = mask.lid_top + (0.86f - mask.lid_top) * lid;
  mask.lid_bottom = mask.lid_bottom + (0.14f - mask.lid_bottom) * lid;
  mask.lid_slant *= 1.0f - lid;
  mask.bottom_curve *= 1.0f - lid;
  prepare(e, &mask, id);
}

static bool render_scene(struct nyabula_eye_renderer_s *r,
                         const struct nyabula_eye_frame_s *frame, int id)
{
  const struct nyabula_eye_scene_frame_s *scene = &frame->scene;
  struct eye_s scene_eye;

  if (scene->scene == NYABULA_EYE_SCENE_NONE)
    {
      return false;
    }

  prepare_scene(&scene_eye, frame, id);
  if (scene->style == NYABULA_EYE_SCENE_STYLE_MINIMAL && scene->lid < 0.999f &&
      scene->alpha < 0.999f)
    {
      struct eye_s mask_eye;
      prepare_scene_lids(&mask_eye, frame, id, scene->lid);
      r->clip_eye = &mask_eye;
      r->clip_lids = true;
      scene_draw_content(r, &scene_eye, scene->scene, &scene->payload,
                         scene->scene_seconds, scene->alpha, scene->reveal,
                         true);
      r->clip_lids = false;
      r->clip_eye = NULL;
      return false;
    }

  if (scene->style == NYABULA_EYE_SCENE_STYLE_MINIMAL)
    {
      disk(r, &scene_eye, scene_eye.t.cx, scene_eye.t.cy, R, 0x000000, 1.0f,
           false);
    }

  if (scene->previous_scene != NYABULA_EYE_SCENE_NONE &&
      scene->previous_alpha > 0.001f)
    {
      scene_draw_content(r, &scene_eye, scene->previous_scene,
                         &scene->previous_payload,
                         scene->previous_scene_seconds, scene->previous_alpha,
                         1.0f,
                         scene->previous_style ==
                             NYABULA_EYE_SCENE_STYLE_MINIMAL);
    }

  scene_draw_content(r, &scene_eye, scene->scene, &scene->payload,
                     scene->scene_seconds, scene->alpha, scene->reveal,
                     scene->style == NYABULA_EYE_SCENE_STYLE_MINIMAL);
  if (scene->style == NYABULA_EYE_SCENE_STYLE_FULL && scene->lid > 0.001f)
    {
      struct eye_s mask_eye;
      prepare_scene_lids(&mask_eye, frame, id, scene->lid);
      lids(r, &mask_eye);
    }

  return true;
}

static void report(struct nyabula_eye_renderer_s *r, uint32_t render_ms)
{
  r->render_total += render_ms;
  if (++r->frames == 300)
    {
      uint32_t elapsed = lv_tick_elaps(r->perf_start);
      LV_LOG_USER("nyabula_eye: %u.%u fps, %u.%02u ms average raster render",
                  300000u / elapsed, (3000000u / elapsed) % 10u,
                  r->render_total / r->frames,
                  (r->render_total * 100u / r->frames) % 100u);
      (void)elapsed;
      r->frames = 0;
      r->render_total = 0;
      r->perf_start = lv_tick_get();
    }
}

#if defined(CONFIG_CONTEST2026_062_NYABULA_DYNAMIC_FONTS) && LV_USE_FREETYPE
static lv_font_t *renderer_create_font(const char *filename, uint32_t size)
{
  char path[256];

  snprintf(path, sizeof(path), "%s/%s",
           CONFIG_CONTEST2026_062_NYABULA_FONT_ROOT, filename);
  return lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                 size, LV_FREETYPE_FONT_STYLE_NORMAL);
}

static void renderer_load_fonts(struct nyabula_eye_renderer_s *r)
{
  r->font_title_20 = renderer_create_font("AlimamaShuHeiTi.ttf", 20);
  r->font_title_28 = renderer_create_font("AlimamaShuHeiTi.ttf", 28);
  r->font_title_42 = renderer_create_font("AlimamaShuHeiTi.ttf", 42);
  r->font_body_14 = renderer_create_font("MiSans-Semibold.ttf", 14);
  r->font_body_18 = renderer_create_font("MiSans-Semibold.ttf", 18);
  r->font_english_18 = renderer_create_font("Tinos-Bold.ttf", 18);
  r->font_english_42 = renderer_create_font("Tinos-Bold.ttf", 42);
  r->font_english_72 = renderer_create_font("Tinos-Bold.ttf", 72);
  r->font_english_96 = renderer_create_font("Tinos-Bold.ttf", 96);
  r->font_english_119 = renderer_create_font("Tinos-Bold.ttf", 119);
}

static void renderer_unload_fonts(struct nyabula_eye_renderer_s *r)
{
  int index;

#define DELETE_FONT(name) \
  do \
    { \
      if (r->font_##name != NULL) \
        { \
          lv_freetype_font_delete(r->font_##name); \
        } \
    } \
  while (0)

  DELETE_FONT(title_20);
  DELETE_FONT(title_28);
  DELETE_FONT(title_42);
  DELETE_FONT(body_14);
  DELETE_FONT(body_18);
  DELETE_FONT(english_18);
  DELETE_FONT(english_42);
  DELETE_FONT(english_72);
  DELETE_FONT(english_96);
  DELETE_FONT(english_119);
#undef DELETE_FONT

  for (index = 0; index < FONT_CACHE_COUNT; index++)
    {
      if (r->font_cache[index].font != NULL)
        {
          lv_freetype_font_delete(r->font_cache[index].font);
          r->font_cache[index].font = NULL;
        }
    }
}
#endif

struct nyabula_eye_renderer_s *nyabula_eye_renderer_create(lv_obj_t *parent)
{
  struct nyabula_eye_renderer_s *r = calloc(1, sizeof(*r));
  if (r == NULL)
    {
      return NULL;
    }

  r->buffer[0] =
      lv_draw_buf_create(W, H, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
  r->buffer[1] =
      lv_draw_buf_create(W, H, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
  r->glyph =
      lv_draw_buf_create(128, 128, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
  if (r->buffer[0] == NULL || r->buffer[1] == NULL || r->glyph == NULL)
    {
      if (r->buffer[0])
        lv_draw_buf_destroy(r->buffer[0]);
      if (r->buffer[1])
        lv_draw_buf_destroy(r->buffer[1]);
      if (r->glyph)
        lv_draw_buf_destroy(r->glyph);
      free(r);
      return NULL;
    }

  r->draw = r->buffer[0];
  r->canvas = lv_canvas_create(parent);
  lv_canvas_set_draw_buf(r->canvas, r->draw);
  lv_obj_set_pos(r->canvas, 0, 0);
  r->random = 0x5a7a5a7au;
  r->perf_start = lv_tick_get();
#if defined(CONFIG_CONTEST2026_062_NYABULA_DYNAMIC_FONTS) && LV_USE_FREETYPE
  renderer_load_fonts(r);
#endif
  return r;
}

void nyabula_eye_renderer_destroy(struct nyabula_eye_renderer_s *r)
{
  if (r != NULL)
    {
#if defined(CONFIG_CONTEST2026_062_NYABULA_DYNAMIC_FONTS) && LV_USE_FREETYPE
      renderer_unload_fonts(r);
#endif
      lv_obj_delete(r->canvas);
      lv_draw_buf_destroy(r->buffer[0]);
      lv_draw_buf_destroy(r->buffer[1]);
      lv_draw_buf_destroy(r->glyph);
      for (int index = 0; index < ICON_CACHE_COUNT; index++)
        {
          free(r->icon_cache[index].coverage);
        }
      free(r);
    }
}

void nyabula_eye_renderer_render(struct nyabula_eye_renderer_s *r,
                                 const struct nyabula_eye_frame_s frames[2])
{
  struct eye_s e;
  lv_color32_t *pixels;
  uint32_t stride;
  uint32_t start;
  int id;
  int x;
  int y;

  if (r == NULL || frames == NULL)
    {
      return;
    }

  start = lv_tick_get();
  r->index ^= 1u;
  r->draw = r->buffer[r->index];
  memset(r->draw->data, 0, r->draw->data_size);
  pixels = (lv_color32_t *)r->draw->data;
  stride = r->draw->header.stride / sizeof(*pixels);
  for (y = 0; y < H; y++)
    {
      for (x = 0; x < W; x++)
        pixels[y * stride + x].alpha = 255;
    }

  update_z(r, &frames[0]);
  for (id = 0; id < 2; id++)
    {
      bool minimal_exit =
          frames[id].scene.scene != NYABULA_EYE_SCENE_NONE &&
          frames[id].scene.style == NYABULA_EYE_SCENE_STYLE_MINIMAL &&
          frames[id].scene.lid < 0.999f && frames[id].scene.alpha < 0.999f;
      if (frames[id].scene.scene != NYABULA_EYE_SCENE_NONE && !minimal_exit)
        {
          render_scene(r, &frames[id], id);
          prepare_scene(&e, &frames[id], id);
          arc(r, &e, 0.0f, 0.0f, R - 0.8f, 0.0f, PI * 2.0f, 2.0f,
              0x3c4655, 0.45f);
          continue;
        }

      if (frames[id].scene.scene == NYABULA_EYE_SCENE_NONE &&
          frames[id].scene.lid > 0.001f)
        {
          prepare_scene_lids(&e, &frames[id], id, frames[id].scene.lid);
        }
      else
        {
          prepare(&e, &frames[id], id);
        }
      base(r, &e);
      iris(r, &e);
      pupil(r, &e);
      overlays(r, &e);
      highlights(r, &e);
      lids(r, &e);
      draw_z(r, &e);
      if (minimal_exit)
        {
          render_scene(r, &frames[id], id);
        }
      arc(r, &e, 0.0f, 0.0f, R - 0.8f, 0.0f, PI * 2.0f, 2.0f, 0x3c4655, 0.45f);
    }

  lv_draw_buf_flush_cache(r->draw, NULL);
  lv_canvas_set_draw_buf(r->canvas, r->draw);
  lv_obj_invalidate(r->canvas);
  report(r, lv_tick_elaps(start));
}
