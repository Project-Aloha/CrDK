/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#include <Library/cmddb.h>
#include <oskal/cr_assert.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_status.h>
#include <oskal/cr_string.h>
#include <oskal/cr_types.h>
#include <oskal/common.h>
#define CMD_DB_MAGIC 0x0c0330db

typedef struct {
  UINT32 Address;
  UINT8 Name[8]; // Aka ID
  UINT32 DataLength;
  UINT8 *Data;
} cmd_db_entry;

static inline const char *rsc_slave_id_to_string(enum RSC_SLAVE_ID_TYPE id)
{
  switch (id) {
  case RSC_SLAVE_HW_INVALID:
    return "INVALID";
  case RSC_SLAVE_HW_ARC:
    return "ARC";
  case RSC_SLAVE_HW_VRM:
    return "VRM";
  case RSC_SLAVE_HW_BCM:
    return "BCM";
  default:
    return "UNKNOWN";
  }
}
