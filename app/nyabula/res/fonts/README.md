# Nyabula complete fonts

Eye Engine keeps the complete font files here because scene payloads may contain
arbitrary user, media, weather, call, task, calendar, subtitle, or agent text.
At runtime LVGL FreeType loads these files from
`CONFIG_CONTEST2026_062_NYABULA_FONT_ROOT`. The generated LVGL subsets under
`src/generated/fonts/` are only the boot-safe fallback used before `/data` is
available.

- `AlimamaShuHeiTi.ttf`: Nyabula Chinese display titles.
- `MiSans-Semibold.ttf`: Chinese and mixed-language body copy.
- `Tinos-Bold.ttf`: metric-compatible open replacement for Times New Roman,
  used by English text and numerals.

The product image must install this directory as `/data/nyabula/fonts` (or set a
different Kconfig path). Keep each upstream font license with release and
distribution artifacts; do not substitute a system-installed proprietary Times
New Roman file.
