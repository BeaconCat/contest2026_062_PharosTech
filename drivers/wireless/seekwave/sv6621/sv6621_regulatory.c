/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_regulatory.c
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

#include <ctype.h>
#include <errno.h>
#include <string.h>

#include "sv6621_regulatory.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_REGULATORY_INSTANCE           0
#define SV6621_REGULATORY_COMMAND_SET_DOMAIN 51
#define SV6621_REGULATORY_COMMAND_TIMEOUT_MS 5000
#define SV6621_REGULATORY_HEADER_SIZE        4
#define SV6621_REGULATORY_RULE_SIZE          8
#define SV6621_REGULATORY_PAYLOAD_SIZE \
  (SV6621_REGULATORY_HEADER_SIZE +      \
   SV6621_REGULATORY_MAX_RULES * SV6621_REGULATORY_RULE_SIZE)

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_regulatory_put_le32(FAR uint8_t *output, uint32_t value);
static bool sv6621_regulatory_country_valid(FAR const char country[2]);
static bool sv6621_regulatory_rule_valid(
    FAR const struct sv6621_regulatory_rule_s *rule);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_regulatory_put_le32
 ****************************************************************************/

static void sv6621_regulatory_put_le32(FAR uint8_t *output, uint32_t value)
{
  output[0] = value & 0xff;
  output[1] = (value >> 8) & 0xff;
  output[2] = (value >> 16) & 0xff;
  output[3] = value >> 24;
}

/****************************************************************************
 * Name: sv6621_regulatory_country_valid
 ****************************************************************************/

static bool sv6621_regulatory_country_valid(FAR const char country[2])
{
  return country != NULL &&
         ((country[0] == '0' && country[1] == '0') ||
          (isalpha((unsigned char)country[0]) &&
           isalpha((unsigned char)country[1])));
}

/****************************************************************************
 * Name: sv6621_regulatory_rule_valid
 ****************************************************************************/

static bool sv6621_regulatory_rule_valid(
    FAR const struct sv6621_regulatory_rule_s *rule)
{
  uint16_t last_channel;

  if (rule == NULL || rule->start_channel == 0 || rule->channel_span == 0)
    {
      return false;
    }

  last_channel = (uint16_t)rule->start_channel + rule->channel_span - 1;
  return last_channel <= UINT8_MAX;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_regulatory_set_domain
 ****************************************************************************/

int sv6621_regulatory_set_domain(
    FAR struct sv6621_command_engine_s *command,
    FAR const struct sv6621_regulatory_domain_s *domain)
{
  uint8_t payload[SV6621_REGULATORY_PAYLOAD_SIZE];
  size_t index;
  int ret;

  if (command == NULL || domain == NULL ||
      !sv6621_regulatory_country_valid(domain->country) ||
      domain->rule_count == 0 ||
      domain->rule_count > SV6621_REGULATORY_MAX_RULES)
    {
      return -EINVAL;
    }

  memset(payload, 0, sizeof(payload));
  payload[0] = toupper((unsigned char)domain->country[0]);
  payload[1] = toupper((unsigned char)domain->country[1]);
  payload[3] = domain->rule_count;

  for (index = 0; index < domain->rule_count; index++)
    {
      FAR const struct sv6621_regulatory_rule_s *rule =
          &domain->rules[index];
      FAR uint8_t *encoded =
          payload + SV6621_REGULATORY_HEADER_SIZE +
          index * SV6621_REGULATORY_RULE_SIZE;

      if (!sv6621_regulatory_rule_valid(rule))
        {
          return -EINVAL;
        }

      encoded[0] = rule->start_channel;
      encoded[1] = rule->channel_span;
      encoded[2] = (uint8_t)rule->max_power_dbm;
      encoded[3] = (uint8_t)rule->max_antenna_gain_dbi;
      sv6621_regulatory_put_le32(encoded + 4, rule->flags);
    }

  ret = sv6621_command_execute(
      command, SV6621_REGULATORY_INSTANCE,
      SV6621_REGULATORY_COMMAND_SET_DOMAIN, payload, sizeof(payload), NULL,
      NULL, SV6621_REGULATORY_COMMAND_TIMEOUT_MS);
  return ret == 0 ? 0 : (ret < 0 ? ret : -EREMOTEIO);
}
