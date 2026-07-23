/****************************************************************************
 * chips/rk3576/rk3576_addrenv.h
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

#ifndef __CHIPS_RK3576_RK3576_ADDRENV_H
#define __CHIPS_RK3576_RK3576_ADDRENV_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <sys/types.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: up_addrenv_va_to_pa
 *
 * Description:
 *   Translate a kernel virtual address to its physical address by walking
 *   the current MMU page tables.  In flat address space (CONFIG_BUILD_FLAT,
 *   the default for rk3576) this is a no-op and returns vaddr unchanged.
 *
 * Input Parameters:
 *   vaddr - Kernel virtual address to translate.
 *
 * Returned Value:
 *   Corresponding physical address, or vaddr if no mapping is found.
 *
 ****************************************************************************/

uintptr_t up_addrenv_va_to_pa(FAR void *vaddr);

#endif /* __CHIPS_RK3576_RK3576_ADDRENV_H */
