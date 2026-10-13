/* Compatibility source path for the standalone Windows provider.
 * The explicit-target ABI and transaction journal are shared with ICBLib.
 * Projects should import interconnect.props, which imports the actual ICB inputs.
 * SPDX-License-Identifier: MIT */
#define CR_OSK_LOG_WITHOUT_WPP 1
#include "../ICBLib/icb.c"
