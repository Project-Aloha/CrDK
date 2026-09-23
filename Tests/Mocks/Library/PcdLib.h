/** @file
  Host-test PCD access shim.

  SPDX-License-Identifier: MIT
**/

#ifndef CRANE_TEST_PCD_LIB_H_
#define CRANE_TEST_PCD_LIB_H_

#include <Uefi.h>

extern UINT64  mTestPciIoTranslation;

#define PcdPciIoTranslation  0
#define PcdGet64(TokenName)   (mTestPciIoTranslation)

#endif
