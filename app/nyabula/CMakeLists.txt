# ##############################################################################
# app/nyabula/CMakeLists.txt
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

if(CONFIG_CONTEST2026_062_NYABULA)
  set(NYABULA_GATEWAY_SRCS)
  if(CONFIG_CONTEST2026_062_NYABULA_GATEWAY)
    list(APPEND NYABULA_GATEWAY_SRCS
         src/nyabula_gateway.c
         src/generated/nyabula_console_html.c)
  endif()

  set_source_files_properties(
    src/nyabula_core.cxx
    PROPERTIES COMPILE_OPTIONS "-std=c++17;-fno-exceptions;-fno-rtti")

  nuttx_add_application(
    NAME
    nyabula
    PRIORITY
    ${CONFIG_CONTEST2026_062_NYABULA_PRIORITY}
    STACKSIZE
    ${CONFIG_CONTEST2026_062_NYABULA_STACKSIZE}
    MODULE
    ${CONFIG_CONTEST2026_062_NYABULA}
    DEPENDS
    lvgl
    SRCS
    src/nyabula_eye_engine.c
    src/nyabula_eye_renderer_lvgl.c
    src/nyabula_core.cxx
    ${NYABULA_GATEWAY_SRCS}
    src/generated/nyabula_eye_icons.c
    src/generated/fonts/nyabula_font_body_14.c
    src/generated/fonts/nyabula_font_body_18.c
    src/generated/fonts/nyabula_font_english_18.c
    src/generated/fonts/nyabula_font_english_42.c
    src/generated/fonts/nyabula_font_english_72.c
    src/generated/fonts/nyabula_font_english_96.c
    src/generated/fonts/nyabula_font_english_119.c
    src/generated/fonts/nyabula_font_title_20.c
    src/generated/fonts/nyabula_font_title_28.c
    src/generated/fonts/nyabula_font_title_42.c
    src/nyabula_main.c)
endif()
