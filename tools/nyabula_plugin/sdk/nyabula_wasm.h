/****************************************************************************
 * tools/nyabula_plugin/sdk/nyabula_wasm.h
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements. See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

#ifndef NYABULA_WASM_H
#define NYABULA_WASM_H

#include <stdint.h>

#if defined(__wasm__)
#define NYABULA_IMPORT(name) \
  __attribute__((import_module("nyabula"), import_name(name)))
#define NYABULA_EXPORT(name) __attribute__((export_name(name)))
#else
#define NYABULA_IMPORT(name)
#define NYABULA_EXPORT(name)
#endif

/* Positive tokens belong to one instance. Negative results are target errno
 * values. Do not truncate tokens to 32 bits or reuse them across instances.
 */
typedef int64_t nyabula_token_t;

NYABULA_IMPORT("core_log")
int32_t nyabula_core_log(const char *message, uint32_t length);

NYABULA_IMPORT("lifecycle_defer")
nyabula_token_t nyabula_lifecycle_defer(void);
NYABULA_IMPORT("lifecycle_complete")
int32_t nyabula_lifecycle_complete(nyabula_token_t token, int32_t result);

NYABULA_IMPORT("http_request")
nyabula_token_t nyabula_http_request(const char *url, uint32_t length);
NYABULA_IMPORT("http_cancel")
int32_t nyabula_http_cancel(nyabula_token_t token);

/* Define this export when importing http_request. Body is borrowed only
 * during this callback and must not be freed. Copy it to retain it.
 * Errors carry no body. Explicit cancellation and stop discard responses.
 */
NYABULA_EXPORT("ny_on_response")
void ny_on_response(nyabula_token_t token, int32_t result, uint32_t status,
                    const uint8_t *body, uint32_t length);

#endif
