/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_transport.c
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
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>

#include "sv6621_transport.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_transport_validate(FAR const struct sv6621_transport_s *transport)
{
  FAR const struct sv6621_transport_ops_s *ops;

  if (transport == NULL || transport->ops == NULL)
    {
      return -EINVAL;
    }

  ops = transport->ops;
  if (ops->open == NULL || ops->enumerate == NULL || ops->close == NULL ||
      ops->read_byte == NULL ||
      ops->write_byte == NULL || ops->read == NULL || ops->write == NULL ||
      ops->attach_irq == NULL || ops->enable_irq == NULL)
    {
      return -ENOSYS;
    }

  return OK;
}
