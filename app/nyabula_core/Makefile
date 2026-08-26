############################################################################
# packages/demos/contest2026_062_nyabula_core/Makefile
# SPDX-License-Identifier: Apache-2.0
############################################################################
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  = nycore
PRIORITY  = $(CONFIG_NYABULA_CORE_PRIORITY)
STACKSIZE = $(CONFIG_NYABULA_CORE_STACKSIZE)
MODULE    = $(CONFIG_CONTEST2026_062_NYABULA_CORE)

MAINSRC = nycore_main.c
CSRCS   = ny_capability.c ny_manifest.c ny_module.c ny_package.c
CSRCS  += ny_permission.c
CSRCS  += ny_runtime.c ny_scheduler.c
CSRCS  += ny_signature.c

include $(APPDIR)/Application.mk
