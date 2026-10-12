/** Native DAL compatibility adapter. SPDX-License-Identifier: MIT */
#include <Library/Sm8450Interconnect.h>

InterconnectTargetContext *
CrTargetGetInterconnectContext(VOID)
{
  return (InterconnectTargetContext *)Sm8450InterconnectGetTarget();
}
