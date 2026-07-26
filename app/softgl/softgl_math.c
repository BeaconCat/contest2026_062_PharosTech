/****************************************************************************
 * app/softgl/softgl_math.c
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
 * SoftGL linear algebra: vec2/vec3/vec4, column-major 4x4 matrices and
 * quaternions.  Matrix conventions follow OpenGL: column-major storage,
 * v' = M * v, right-handed eye space looking down -Z, and a projection that
 * maps the view frustum to NDC z in [-1, +1].
 *
 * The 4x4 by vec4 product has an AArch64 NEON fast path; everything else is
 * scalar because it runs once per object, not once per vertex.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <math.h>
#include <string.h>

#include "softgl.h"

#ifdef __ARM_NEON
#  include <arm_neon.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Below this squared length a vector is treated as degenerate. */

#define SOFTGL_EPSILON_SQ         1e-20f

/* Determinants smaller than this make a matrix non invertible. */

#define SOFTGL_DET_EPSILON        1e-12f

/* Above this dot product slerp degenerates into a linear interpolation. */

#define SOFTGL_SLERP_LINEAR_DOT   0.9995f

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: softgl_vec2 / softgl_vec3 / softgl_vec4
 ****************************************************************************/

struct softgl_vec2_s softgl_vec2(float x, float y)
{
  struct softgl_vec2_s r;

  r.x = x;
  r.y = y;
  return r;
}

struct softgl_vec3_s softgl_vec3(float x, float y, float z)
{
  struct softgl_vec3_s r;

  r.x = x;
  r.y = y;
  r.z = z;
  return r;
}

struct softgl_vec4_s softgl_vec4(float x, float y, float z, float w)
{
  struct softgl_vec4_s r;

  r.x = x;
  r.y = y;
  r.z = z;
  r.w = w;
  return r;
}

/****************************************************************************
 * Name: softgl_vec3_add / sub / scale / cross / dot / length / normalize
 ****************************************************************************/

struct softgl_vec3_s softgl_vec3_add(struct softgl_vec3_s a,
                                     struct softgl_vec3_s b)
{
  return softgl_vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

struct softgl_vec3_s softgl_vec3_sub(struct softgl_vec3_s a,
                                     struct softgl_vec3_s b)
{
  return softgl_vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

struct softgl_vec3_s softgl_vec3_scale(struct softgl_vec3_s a, float s)
{
  return softgl_vec3(a.x * s, a.y * s, a.z * s);
}

struct softgl_vec3_s softgl_vec3_cross(struct softgl_vec3_s a,
                                       struct softgl_vec3_s b)
{
  return softgl_vec3(a.y * b.z - a.z * b.y,
                     a.z * b.x - a.x * b.z,
                     a.x * b.y - a.y * b.x);
}

float softgl_vec3_dot(struct softgl_vec3_s a, struct softgl_vec3_s b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

float softgl_vec3_length(struct softgl_vec3_s a)
{
  return sqrtf(softgl_vec3_dot(a, a));
}

struct softgl_vec3_s softgl_vec3_normalize(struct softgl_vec3_s a)
{
  float len2 = softgl_vec3_dot(a, a);

  if (len2 < SOFTGL_EPSILON_SQ)
    {
      return softgl_vec3(0.0f, 0.0f, 0.0f);
    }

  return softgl_vec3_scale(a, 1.0f / sqrtf(len2));
}

/****************************************************************************
 * Name: softgl_mat4_identity
 ****************************************************************************/

void softgl_mat4_identity(struct softgl_mat4_s *out)
{
  memset(out->m, 0, sizeof(out->m));
  out->m[0]  = 1.0f;
  out->m[5]  = 1.0f;
  out->m[10] = 1.0f;
  out->m[15] = 1.0f;
}

/****************************************************************************
 * Name: softgl_mat4_mul
 *
 * Description:
 *   out = a * b.  Aliasing between out and the operands is allowed.
 *
 ****************************************************************************/

void softgl_mat4_mul(struct softgl_mat4_s *out,
                     const struct softgl_mat4_s *a,
                     const struct softgl_mat4_s *b)
{
  struct softgl_mat4_s tmp;
  int col;
  int row;

  for (col = 0; col < 4; col++)
    {
      for (row = 0; row < 4; row++)
        {
          tmp.m[col * 4 + row] = a->m[0 * 4 + row] * b->m[col * 4 + 0] +
                                 a->m[1 * 4 + row] * b->m[col * 4 + 1] +
                                 a->m[2 * 4 + row] * b->m[col * 4 + 2] +
                                 a->m[3 * 4 + row] * b->m[col * 4 + 3];
        }
    }

  *out = tmp;
}

/****************************************************************************
 * Name: softgl_mat4_mul_vec4
 *
 * Description:
 *   Transform a homogeneous vector.  This is the hot call of the vertex
 *   stage, so it uses NEON when available: with column-major storage the
 *   product is simply a linear combination of the four columns, which maps
 *   directly onto four fused multiply-adds on 4-lane vectors.
 *
 ****************************************************************************/

struct softgl_vec4_s softgl_mat4_mul_vec4(const struct softgl_mat4_s *m,
                                          struct softgl_vec4_s v)
{
  struct softgl_vec4_s r;

#ifdef __ARM_NEON
  float32x4_t c0 = vld1q_f32(&m->m[0]);
  float32x4_t c1 = vld1q_f32(&m->m[4]);
  float32x4_t c2 = vld1q_f32(&m->m[8]);
  float32x4_t c3 = vld1q_f32(&m->m[12]);
  float32x4_t acc;

  acc = vmulq_n_f32(c0, v.x);
  acc = vmlaq_n_f32(acc, c1, v.y);
  acc = vmlaq_n_f32(acc, c2, v.z);
  acc = vmlaq_n_f32(acc, c3, v.w);

  vst1q_f32((float *)&r, acc);
#else
  r.x = m->m[0] * v.x + m->m[4] * v.y + m->m[8]  * v.z + m->m[12] * v.w;
  r.y = m->m[1] * v.x + m->m[5] * v.y + m->m[9]  * v.z + m->m[13] * v.w;
  r.z = m->m[2] * v.x + m->m[6] * v.y + m->m[10] * v.z + m->m[14] * v.w;
  r.w = m->m[3] * v.x + m->m[7] * v.y + m->m[11] * v.z + m->m[15] * v.w;
#endif

  return r;
}

/****************************************************************************
 * Name: softgl_mat4_mul_point
 *
 * Description:
 *   Transform a position (w = 1) and drop the resulting w.
 *
 ****************************************************************************/

struct softgl_vec3_s softgl_mat4_mul_point(const struct softgl_mat4_s *m,
                                           struct softgl_vec3_s v)
{
  struct softgl_vec4_s r =
    softgl_mat4_mul_vec4(m, softgl_vec4(v.x, v.y, v.z, 1.0f));

  return softgl_vec3(r.x, r.y, r.z);
}

/****************************************************************************
 * Name: softgl_mat4_mul_dir
 *
 * Description:
 *   Transform a direction (w = 0), i.e. ignore the translation column.
 *
 ****************************************************************************/

struct softgl_vec3_s softgl_mat4_mul_dir(const struct softgl_mat4_s *m,
                                         struct softgl_vec3_s v)
{
  struct softgl_vec4_s r =
    softgl_mat4_mul_vec4(m, softgl_vec4(v.x, v.y, v.z, 0.0f));

  return softgl_vec3(r.x, r.y, r.z);
}

/****************************************************************************
 * Name: softgl_mat4_transpose
 ****************************************************************************/

void softgl_mat4_transpose(struct softgl_mat4_s *out,
                           const struct softgl_mat4_s *a)
{
  struct softgl_mat4_s tmp;
  int col;
  int row;

  for (col = 0; col < 4; col++)
    {
      for (row = 0; row < 4; row++)
        {
          tmp.m[col * 4 + row] = a->m[row * 4 + col];
        }
    }

  *out = tmp;
}

/****************************************************************************
 * Name: softgl_mat4_inverse
 *
 * Description:
 *   General 4x4 inverse by cofactor expansion.  Returns false (and leaves
 *   out untouched) when the matrix is singular.
 *
 ****************************************************************************/

bool softgl_mat4_inverse(struct softgl_mat4_s *out,
                         const struct softgl_mat4_s *a)
{
  const float *s = a->m;
  float inv[16];
  float det;
  int i;

  inv[0]  =  s[5] * s[10] * s[15] - s[5]  * s[11] * s[14] -
             s[9] * s[6]  * s[15] + s[9]  * s[7]  * s[14] +
             s[13] * s[6] * s[11] - s[13] * s[7]  * s[10];

  inv[4]  = -s[4] * s[10] * s[15] + s[4]  * s[11] * s[14] +
             s[8] * s[6]  * s[15] - s[8]  * s[7]  * s[14] -
             s[12] * s[6] * s[11] + s[12] * s[7]  * s[10];

  inv[8]  =  s[4] * s[9]  * s[15] - s[4]  * s[11] * s[13] -
             s[8] * s[5]  * s[15] + s[8]  * s[7]  * s[13] +
             s[12] * s[5] * s[11] - s[12] * s[7]  * s[9];

  inv[12] = -s[4] * s[9]  * s[14] + s[4]  * s[10] * s[13] +
             s[8] * s[5]  * s[14] - s[8]  * s[6]  * s[13] -
             s[12] * s[5] * s[10] + s[12] * s[6]  * s[9];

  inv[1]  = -s[1] * s[10] * s[15] + s[1]  * s[11] * s[14] +
             s[9] * s[2]  * s[15] - s[9]  * s[3]  * s[14] -
             s[13] * s[2] * s[11] + s[13] * s[3]  * s[10];

  inv[5]  =  s[0] * s[10] * s[15] - s[0]  * s[11] * s[14] -
             s[8] * s[2]  * s[15] + s[8]  * s[3]  * s[14] +
             s[12] * s[2] * s[11] - s[12] * s[3]  * s[10];

  inv[9]  = -s[0] * s[9]  * s[15] + s[0]  * s[11] * s[13] +
             s[8] * s[1]  * s[15] - s[8]  * s[3]  * s[13] -
             s[12] * s[1] * s[11] + s[12] * s[3]  * s[9];

  inv[13] =  s[0] * s[9]  * s[14] - s[0]  * s[10] * s[13] -
             s[8] * s[1]  * s[14] + s[8]  * s[2]  * s[13] +
             s[12] * s[1] * s[10] - s[12] * s[2]  * s[9];

  inv[2]  =  s[1] * s[6]  * s[15] - s[1]  * s[7]  * s[14] -
             s[5] * s[2]  * s[15] + s[5]  * s[3]  * s[14] +
             s[13] * s[2] * s[7]  - s[13] * s[3]  * s[6];

  inv[6]  = -s[0] * s[6]  * s[15] + s[0]  * s[7]  * s[14] +
             s[4] * s[2]  * s[15] - s[4]  * s[3]  * s[14] -
             s[12] * s[2] * s[7]  + s[12] * s[3]  * s[6];

  inv[10] =  s[0] * s[5]  * s[15] - s[0]  * s[7]  * s[13] -
             s[4] * s[1]  * s[15] + s[4]  * s[3]  * s[13] +
             s[12] * s[1] * s[7]  - s[12] * s[3]  * s[5];

  inv[14] = -s[0] * s[5]  * s[14] + s[0]  * s[6]  * s[13] +
             s[4] * s[1]  * s[14] - s[4]  * s[2]  * s[13] -
             s[12] * s[1] * s[6]  + s[12] * s[2]  * s[5];

  inv[3]  = -s[1] * s[6]  * s[11] + s[1]  * s[7]  * s[10] +
             s[5] * s[2]  * s[11] - s[5]  * s[3]  * s[10] -
             s[9] * s[2]  * s[7]  + s[9]  * s[3]  * s[6];

  inv[7]  =  s[0] * s[6]  * s[11] - s[0]  * s[7]  * s[10] -
             s[4] * s[2]  * s[11] + s[4]  * s[3]  * s[10] +
             s[8] * s[2]  * s[7]  - s[8]  * s[3]  * s[6];

  inv[11] = -s[0] * s[5]  * s[11] + s[0]  * s[7]  * s[9] +
             s[4] * s[1]  * s[11] - s[4]  * s[3]  * s[9] -
             s[8] * s[1]  * s[7]  + s[8]  * s[3]  * s[5];

  inv[15] =  s[0] * s[5]  * s[10] - s[0]  * s[6]  * s[9] -
             s[4] * s[1]  * s[10] + s[4]  * s[2]  * s[9] +
             s[8] * s[1]  * s[6]  - s[8]  * s[2]  * s[5];

  det = s[0] * inv[0] + s[1] * inv[4] + s[2] * inv[8] + s[3] * inv[12];
  if (det > -SOFTGL_DET_EPSILON && det < SOFTGL_DET_EPSILON)
    {
      return false;
    }

  det = 1.0f / det;
  for (i = 0; i < 16; i++)
    {
      out->m[i] = inv[i] * det;
    }

  return true;
}

/****************************************************************************
 * Name: softgl_mat4_translate
 ****************************************************************************/

void softgl_mat4_translate(struct softgl_mat4_s *out, float x, float y,
                           float z)
{
  softgl_mat4_identity(out);
  out->m[12] = x;
  out->m[13] = y;
  out->m[14] = z;
}

/****************************************************************************
 * Name: softgl_mat4_scale
 ****************************************************************************/

void softgl_mat4_scale(struct softgl_mat4_s *out, float x, float y, float z)
{
  softgl_mat4_identity(out);
  out->m[0]  = x;
  out->m[5]  = y;
  out->m[10] = z;
}

/****************************************************************************
 * Name: softgl_mat4_rotate
 *
 * Description:
 *   Right-handed rotation of "radians" about an arbitrary axis
 *   (Rodrigues' formula written out as a matrix).
 *
 ****************************************************************************/

void softgl_mat4_rotate(struct softgl_mat4_s *out, struct softgl_vec3_s axis,
                        float radians)
{
  struct softgl_vec3_s a = softgl_vec3_normalize(axis);
  float c = cosf(radians);
  float s = sinf(radians);
  float t = 1.0f - c;

  softgl_mat4_identity(out);

  out->m[0]  = t * a.x * a.x + c;
  out->m[1]  = t * a.x * a.y + s * a.z;
  out->m[2]  = t * a.x * a.z - s * a.y;

  out->m[4]  = t * a.x * a.y - s * a.z;
  out->m[5]  = t * a.y * a.y + c;
  out->m[6]  = t * a.y * a.z + s * a.x;

  out->m[8]  = t * a.x * a.z + s * a.y;
  out->m[9]  = t * a.y * a.z - s * a.x;
  out->m[10] = t * a.z * a.z + c;
}

/****************************************************************************
 * Name: softgl_mat4_perspective
 *
 * Description:
 *   Right-handed perspective projection mapping [znear, zfar] to NDC z in
 *   [-1, +1].  zfar may be infinite in spirit but must be finite here.
 *
 ****************************************************************************/

void softgl_mat4_perspective(struct softgl_mat4_s *out, float fovy_radians,
                             float aspect, float znear, float zfar)
{
  float f = 1.0f / tanf(fovy_radians * 0.5f);
  float nf = 1.0f / (znear - zfar);

  memset(out->m, 0, sizeof(out->m));
  out->m[0]  = f / aspect;
  out->m[5]  = f;
  out->m[10] = (zfar + znear) * nf;
  out->m[11] = -1.0f;
  out->m[14] = 2.0f * zfar * znear * nf;
}

/****************************************************************************
 * Name: softgl_mat4_ortho
 ****************************************************************************/

void softgl_mat4_ortho(struct softgl_mat4_s *out, float left, float right,
                       float bottom, float top, float znear, float zfar)
{
  float rl = 1.0f / (right - left);
  float tb = 1.0f / (top - bottom);
  float fn = 1.0f / (zfar - znear);

  memset(out->m, 0, sizeof(out->m));
  out->m[0]  =  2.0f * rl;
  out->m[5]  =  2.0f * tb;
  out->m[10] = -2.0f * fn;
  out->m[12] = -(right + left) * rl;
  out->m[13] = -(top + bottom) * tb;
  out->m[14] = -(zfar + znear) * fn;
  out->m[15] =  1.0f;
}

/****************************************************************************
 * Name: softgl_mat4_lookat
 *
 * Description:
 *   Right-handed view matrix: the camera sits at "eye" and looks towards
 *   "center" down its local -Z axis.
 *
 ****************************************************************************/

void softgl_mat4_lookat(struct softgl_mat4_s *out, struct softgl_vec3_s eye,
                        struct softgl_vec3_s center, struct softgl_vec3_s up)
{
  struct softgl_vec3_s f = softgl_vec3_normalize(
                             softgl_vec3_sub(center, eye));
  struct softgl_vec3_s s = softgl_vec3_normalize(softgl_vec3_cross(f, up));
  struct softgl_vec3_s u = softgl_vec3_cross(s, f);

  softgl_mat4_identity(out);

  out->m[0]  =  s.x;
  out->m[4]  =  s.y;
  out->m[8]  =  s.z;

  out->m[1]  =  u.x;
  out->m[5]  =  u.y;
  out->m[9]  =  u.z;

  out->m[2]  = -f.x;
  out->m[6]  = -f.y;
  out->m[10] = -f.z;

  out->m[12] = -softgl_vec3_dot(s, eye);
  out->m[13] = -softgl_vec3_dot(u, eye);
  out->m[14] =  softgl_vec3_dot(f, eye);
}

/****************************************************************************
 * Name: softgl_quat_identity
 ****************************************************************************/

struct softgl_quat_s softgl_quat_identity(void)
{
  struct softgl_quat_s q;

  q.x = 0.0f;
  q.y = 0.0f;
  q.z = 0.0f;
  q.w = 1.0f;
  return q;
}

/****************************************************************************
 * Name: softgl_quat_axis_angle
 ****************************************************************************/

struct softgl_quat_s softgl_quat_axis_angle(struct softgl_vec3_s axis,
                                            float radians)
{
  struct softgl_vec3_s a = softgl_vec3_normalize(axis);
  float half = radians * 0.5f;
  float s = sinf(half);
  struct softgl_quat_s q;

  q.x = a.x * s;
  q.y = a.y * s;
  q.z = a.z * s;
  q.w = cosf(half);
  return q;
}

/****************************************************************************
 * Name: softgl_quat_mul
 *
 * Description:
 *   Hamilton product; the result applies b first, then a.
 *
 ****************************************************************************/

struct softgl_quat_s softgl_quat_mul(struct softgl_quat_s a,
                                     struct softgl_quat_s b)
{
  struct softgl_quat_s q;

  q.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
  q.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
  q.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
  q.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
  return q;
}

/****************************************************************************
 * Name: softgl_quat_normalize
 ****************************************************************************/

struct softgl_quat_s softgl_quat_normalize(struct softgl_quat_s q)
{
  float len2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  float inv;

  if (len2 < SOFTGL_EPSILON_SQ)
    {
      return softgl_quat_identity();
    }

  inv = 1.0f / sqrtf(len2);
  q.x *= inv;
  q.y *= inv;
  q.z *= inv;
  q.w *= inv;
  return q;
}

/****************************************************************************
 * Name: softgl_quat_slerp
 ****************************************************************************/

struct softgl_quat_s softgl_quat_slerp(struct softgl_quat_s a,
                                       struct softgl_quat_s b, float t)
{
  float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  float theta;
  float sin_theta;
  float wa;
  float wb;
  struct softgl_quat_s q;

  /* Take the short way round the hypersphere. */

  if (dot < 0.0f)
    {
      b.x = -b.x;
      b.y = -b.y;
      b.z = -b.z;
      b.w = -b.w;
      dot = -dot;
    }

  if (dot > SOFTGL_SLERP_LINEAR_DOT)
    {
      /* Nearly parallel: lerp to avoid dividing by ~0. */

      wa = 1.0f - t;
      wb = t;
    }
  else
    {
      theta     = acosf(dot);
      sin_theta = sinf(theta);
      wa        = sinf((1.0f - t) * theta) / sin_theta;
      wb        = sinf(t * theta) / sin_theta;
    }

  q.x = a.x * wa + b.x * wb;
  q.y = a.y * wa + b.y * wb;
  q.z = a.z * wa + b.z * wb;
  q.w = a.w * wa + b.w * wb;
  return softgl_quat_normalize(q);
}

/****************************************************************************
 * Name: softgl_quat_to_mat4
 ****************************************************************************/

void softgl_quat_to_mat4(struct softgl_mat4_s *out, struct softgl_quat_s q)
{
  float xx;
  float yy;
  float zz;
  float xy;
  float xz;
  float yz;
  float wx;
  float wy;
  float wz;

  q = softgl_quat_normalize(q);

  xx = q.x * q.x;
  yy = q.y * q.y;
  zz = q.z * q.z;
  xy = q.x * q.y;
  xz = q.x * q.z;
  yz = q.y * q.z;
  wx = q.w * q.x;
  wy = q.w * q.y;
  wz = q.w * q.z;

  softgl_mat4_identity(out);

  out->m[0]  = 1.0f - 2.0f * (yy + zz);
  out->m[1]  =        2.0f * (xy + wz);
  out->m[2]  =        2.0f * (xz - wy);

  out->m[4]  =        2.0f * (xy - wz);
  out->m[5]  = 1.0f - 2.0f * (xx + zz);
  out->m[6]  =        2.0f * (yz + wx);

  out->m[8]  =        2.0f * (xz + wy);
  out->m[9]  =        2.0f * (yz - wx);
  out->m[10] = 1.0f - 2.0f * (xx + yy);
}
