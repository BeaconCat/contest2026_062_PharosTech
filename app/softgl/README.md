# SoftGL —— 纯 CPU 软件 3D 光栅化渲染器

GPU 路线 2 的落地实现：**零 GPU 依赖、纯 openvela/NuttX**，在 RK3576 的
Cortex-A72×4 + A53×4 上跑完整软件 3D 管线，目标 KICKPI-K7 双圆屏 360×360
「猫舍模式」。

对应文档：`工具/GPU图形路线.md` 路 2。

## 文件

| 文件 | 内容 |
|---|---|
| `softgl.h` | 全部公共 API：数学类型、mesh/texture/context、渲染状态 |
| `softgl_math.c` | vec2/3/4、列主序 mat4（乘/转置/求逆/perspective/ortho/lookAt）、四元数（axis-angle/mul/slerp/to_mat4）。mat4×vec4 走 NEON |
| `softgl_raster.c` | 顶点变换 → 近平面裁剪 → 投影 → 背面剔除 → 三角形分箱 → 分带多线程扫描转换 |
| `softgl.c` | 上下文/缓冲/线程池、/dev/fb0 呈现、PPM 导出、.mesh 与 OBJ 加载 |
| `softgl_demo.c` | NSH 应用 `softgl`：旋转低多边形猫/立方体，打印 fps |
| `../../tools/obj_to_mesh.py` | OBJ → 紧凑二进制 `.mesh`（也可导出 C 数组内嵌进镜像） |

## 管线

```
mesh (indexed tri)
  └ 顶点级：MVP 变换(NEON) + 世界坐标/世界法线(逆转置)  ── 单线程
  └ 三角形级：近平面 Sutherland-Hodgman 裁剪 → 透视除法 → 视口变换
              → 背面剔除 → 屏幕空间三角形分箱(varying 预除 w)  ── 单线程
  └ 像素级：分带并行，边函数半空间覆盖 + top-left 填充规则
            → 重心坐标 → 16bit Z-buffer early-Z → 透视正确插值
            → RGB565 纹理(最近邻/双线性) → 环境 + Lambert + Blinn-Phong
            → 打包 RGB565                                  ── N 线程
```

**并行模型**：帧缓冲按扫描线切成 N 条水平带，每线程一条。每个线程遍历
整个三角形箱，但把包围盒裁到自己的带里 —— 颜色/深度缓冲按构造互不相交，
内层循环**零加锁**。已实测：4 线程输出与单线程**逐字节一致**。

## API 速览

```c
struct softgl_context_s *ctx = softgl_create_context(360, 360, NULL);
softgl_bind_fbdev(ctx, "/dev/fb0");          /* 可选，上屏 */
softgl_set_threads(ctx, 4);                  /* 默认 = SMP 核数 */

softgl_set_matrix(ctx, SOFTGL_MATRIX_PROJECTION, &proj);
softgl_set_matrix(ctx, SOFTGL_MATRIX_VIEW, &view);
softgl_set_matrix(ctx, SOFTGL_MATRIX_MODEL, &model);
softgl_bind_texture(ctx, &tex);              /* RGB565 */
softgl_set_light(ctx, &light);

softgl_clear(ctx, SOFTGL_RGB565(18, 16, 24), true);
softgl_draw_mesh(ctx, &mesh);
softgl_present(ctx);
```

## 跑法

```
nsh> softgl                       # 默认 360x360 猫，120 帧，上 /dev/fb0
nsh> softgl -s cube -f 0          # 棋盘立方体 + 最近邻（看透视正确性）
nsh> softgl -x -n 300 -t 8        # 不碰帧缓冲，纯跑分，8 线程
nsh> softgl -m /data/cat.mesh     # 加载离线转换的模型
nsh> softgl -o /data/frame.ppm    # 末帧导出 PPM
```

`-l 0|1|2` 切 unlit / Lambert / Blinn-Phong；`-r` 改分辨率；`-h` 看全部选项。

## 模型转换

```
tools/obj_to_mesh.py cat.obj cat.mesh --normalize --scale 1.6 --flat
tools/obj_to_mesh.py cat.obj --c-header cat_mesh.h --symbol g_cat_mesh
```

`.mesh` 布局与 `struct softgl_mesh_header_s` 逐字节对应（小端，40 字节头 +
`nvertices` 个 32 字节顶点 + `nindices` 个 uint16）。索引是 16 位，模型上限
65535 顶点 —— 远高于 CPU 光栅器的实时预算。

## 内存

360×360 下：颜色缓冲 253 KB（RGB565）+ 深度缓冲 253 KB（uint16，比 float
省一半）+ 三角形箱按需增长。直接把 `/dev/fb0` 的 `fbmem` 传给
`softgl_create_context()` 可省掉呈现时的一次全屏拷贝。
