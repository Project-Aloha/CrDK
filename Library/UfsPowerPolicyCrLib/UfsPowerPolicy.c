/* Board-qualified FDT supply selection. SPDX-License-Identifier: MIT */
#include <Library/UfsPowerPolicyCrLib.h>

STATIC BOOLEAN Representable(CONST CR_TARGET_UFS_POWER_RULE *R, UINT32 Uv)
{
  return Uv >= R->MinUv && Uv <= R->MaxUv && Uv >= R->BaseUv &&
         Uv % 1000 == 0 && (Uv - R->BaseUv) % R->StepUv == 0;
}

STATIC BOOLEAN ResourceEqual(CONST CHAR8 *A, CONST CHAR8 *B)
{
  for (UINT32 I=0; I<CR_UFS_POWER_RESOURCE_SIZE; ++I) {
    if (A[I]!=B[I]) return FALSE;
    if (!A[I]) return TRUE;
  }
  return FALSE;
}

EFI_STATUS CrUfsPowerResolve(CONST CR_TARGET_UFS_CONTEXT *Target,
                            CONST CR_UFS_POWER_FDT *Info,
                            CR_UFS_POWER_RESOLVED *Result)
{
  CONST CR_TARGET_UFS_POWER_POLICY *P;
  CR_UFS_POWER_RESOLVED Out = {0};
  UINT32 Low[CR_TARGET_UFS_MAX_RAILS]={0}, High[CR_TARGET_UFS_MAX_RAILS]={0};
  UINT32 Init[CR_TARGET_UFS_MAX_RAILS]={0};
  BOOLEAN Used[CR_TARGET_UFS_MAX_RAILS]={0};
  BOOLEAN Retained[CR_TARGET_UFS_MAX_RAILS]={0};
  if (!Result || !Info || !CrTargetUfsValid(Target)) return EFI_INVALID_PARAMETER;
  P=Target->PowerPolicy;
  if (!P || (P->Revision!=1 && P->Revision!=2) || P->Size!=sizeof(*P) || !P->Rules ||
      !P->RuleCount || P->RuleCount>CR_TARGET_UFS_MAX_RAILS || P->RequireHob>1 || (P->RetainedRoles & ~0x3fU))
    return EFI_UNSUPPORTED;
  if (P->Revision==2 && (!P->RequireHob || P->VccqParentUv ||
                        !CrTargetUfsString(P->VccqParent))) return EFI_UNSUPPORTED;
  if (Info->Version!=CR_UFS_POWER_FDT_VERSION || Info->Size!=sizeof(*Info) ||
      Info->HostBase!=Target->HciBase || Info->HostSize!=Target->HciBytes ||
      Info->PhyBase!=Target->PhyBase ||
      (Info->PhySize!=0xe00 && Info->PhySize!=0xe10 && Info->PhySize!=0x1000))
    return EFI_COMPROMISED_DATA;
  /* Revision 1 qualifies the existing parent topology. Revision 2 admits
     only the named, optional enable-only supplier. Neither may derive a
     runtime voltage request from provider init metadata. */
  CONST CR_UFS_POWER_SUPPLY *Parent=&Info->VccqParent;
  if (Parent->Kind==CrUfsPowerAbsentInherited) {
    CONST UINT8 *Bytes=(CONST UINT8 *)Parent;
    for (UINTN I=0; I<sizeof(*Parent); ++I)
      if (Bytes[I]) return EFI_COMPROMISED_DATA;
  } else {
    if (Parent->Kind!=CrUfsPowerRpmh || !P->VccqParent ||
        !ResourceEqual(Parent->Resource,P->VccqParent) || !Parent->Phandle ||
        Parent->Phandle==MAX_UINT32 || Parent->ProviderBase || Parent->ProviderSize ||
        !(Parent->Flags&CR_UFS_POWER_RANGE_PRESENT) ||
        (Parent->Flags & ~(CR_UFS_POWER_RANGE_PRESENT|CR_UFS_POWER_INIT_PRESENT|
                          CR_UFS_POWER_BOOT_ON|CR_UFS_POWER_MAX_UA_PRESENT|
                          CR_UFS_POWER_VOTE_SET_PRESENT)) ||
        ((Parent->Flags&CR_UFS_POWER_VOTE_SET_PRESENT) && Parent->VoteSet!=3) ||
        Parent->ConsumerMinUv || Parent->ConsumerMaxUv || !Parent->MinUv ||
        Parent->MinUv>Parent->MaxUv || Parent->MaxUv>5000000 ||
        (P->Revision==1 && (P->VccqParentUv<Parent->MinUv || P->VccqParentUv>Parent->MaxUv)) ||
        ((Parent->Flags&CR_UFS_POWER_INIT_PRESENT) &&
         (Parent->InitUv<Parent->MinUv || Parent->InitUv>Parent->MaxUv)) ||
        (!(Parent->Flags&CR_UFS_POWER_INIT_PRESENT) && Parent->InitUv))
      return EFI_UNSUPPORTED;
    if (P->Revision==2) Out.RetainedEnableSupply=P->VccqParent;
  }
  for (UINT32 I=0; I<P->RuleCount; ++I) {
    CONST CR_TARGET_UFS_POWER_RULE *R=&P->Rules[I];
    if (!CrTargetUfsString(R->Name) || !R->Roles || (R->Roles & ~0x3fU) ||
        !R->MinUv || R->MinUv>R->MaxUv || R->MaxUv>5000000 || !R->StepUv ||
        R->Mode!=7 || !Representable(R,R->PreferredUv)) return EFI_UNSUPPORTED;
    if (Out.RetainedEnableSupply && ResourceEqual(R->Name,Out.RetainedEnableSupply))
      return EFI_UNSUPPORTED;
    for (UINT32 J=0; J<I; ++J)
      if (ResourceEqual(R->Name,P->Rules[J].Name)) return EFI_UNSUPPORTED;
  }
  for (UINT32 Role=0; Role<CR_UFS_POWER_SUPPLY_COUNT; ++Role) {
    CONST CR_UFS_POWER_SUPPLY *S=&Info->Supplies[Role];
    UINT32 Index, Lo, Hi;
    if (S->Flags & ~0x7ffU) return EFI_COMPROMISED_DATA;
    if (S->Kind==CrUfsPowerAbsentInherited) {
      if (Role>=CrUfsPowerVddaPhy || S->Flags || S->Phandle || S->Resource[0] ||
          S->MinUv || S->MaxUv || S->InitUv || S->ConsumerMinUv ||
          S->ConsumerMaxUv || S->ProviderBase || S->ProviderSize || S->VoteSet || S->MaxUa)
        return EFI_COMPROMISED_DATA;
      Out.InheritedRoles |= 1U<<Role;
      continue;
    }
    if (!S->Phandle || S->Phandle==MAX_UINT32) return EFI_COMPROMISED_DATA;
    if (S->Kind==CrUfsPowerGdsc) {
      if (Role!=CrUfsPowerVddHba || S->ProviderBase!=P->GdscBase ||
          !P->GdscBase || S->ProviderSize!=4 || S->Resource[0] ||
          S->MinUv || S->MaxUv || S->InitUv) return EFI_UNSUPPORTED;
      continue; /* The existing target GDSC sequence owns this domain. */
    }
    if (S->Kind==CrUfsPowerFixedAlwaysOn) {
      if (Role!=CrUfsPowerVccq2 || !P->FixedVccq2Uv ||
          !(S->Flags&CR_UFS_POWER_ALWAYS_ON) || !(S->Flags&CR_UFS_POWER_RANGE_PRESENT) ||
          S->MinUv!=P->FixedVccq2Uv || S->MaxUv!=S->MinUv || S->Resource[0] ||
          S->ProviderBase || S->ProviderSize) return EFI_UNSUPPORTED;
      Out.InheritedRoles |= 1U<<Role;
      continue;
    }
    if (S->Kind!=CrUfsPowerRpmh || !(S->Flags&CR_UFS_POWER_RANGE_PRESENT) ||
        !S->MinUv || S->MinUv>S->MaxUv || S->MaxUv>5000000 ||
        S->ProviderBase || S->ProviderSize ||
        (S->Flags&CR_UFS_POWER_CONSUMER_FIXED) ||
        ((S->Flags&(CR_UFS_POWER_ALWAYS_ON|CR_UFS_POWER_CONSUMER_ALWAYS_ON)) &&
         !(P->RetainedRoles & (1U<<Role))) ||
        ((S->Flags&CR_UFS_POWER_VOTE_SET_PRESENT) && S->VoteSet!=3))
      return EFI_UNSUPPORTED;
    for (Index=0; Index<P->RuleCount; ++Index)
      if (ResourceEqual(S->Resource,P->Rules[Index].Name)) break;
    if (Index==P->RuleCount || !(P->Rules[Index].Roles & (1U<<Role))) return EFI_UNSUPPORTED;
    CONST CR_TARGET_UFS_POWER_RULE *R=&P->Rules[Index];
    /* An explicit but physically invalid init is corruption, not permission
       to hide it behind a preferred vote. An init outside only the consumer
       interval is different: Linux also changes it when the device votes. */
    if (S->Flags&CR_UFS_POWER_INIT_PRESENT) {
      if (S->InitUv<S->MinUv || S->InitUv>S->MaxUv || !Representable(R,S->InitUv))
        return EFI_UNSUPPORTED;
      if (Init[Index] && Init[Index]!=S->InitUv) return EFI_COMPROMISED_DATA;
      Init[Index]=S->InitUv;
    } else if (S->InitUv) return EFI_COMPROMISED_DATA;
    Lo=MAX(S->MinUv,R->MinUv); Hi=MIN(S->MaxUv,R->MaxUv);
    if (S->Flags&CR_UFS_POWER_CONSUMER_RANGE) {
      if (!S->ConsumerMinUv || S->ConsumerMinUv>S->ConsumerMaxUv)
        return EFI_COMPROMISED_DATA;
      Lo=MAX(Lo,S->ConsumerMinUv); Hi=MIN(Hi,S->ConsumerMaxUv);
    } else if (S->ConsumerMinUv || S->ConsumerMaxUv) return EFI_COMPROMISED_DATA;
    if (S->Flags&CR_UFS_POWER_VCC_1P8) {
      if (Role!=CrUfsPowerVcc) return EFI_COMPROMISED_DATA;
      Lo=MAX(Lo,1700000U); Hi=MIN(Hi,1950000U);
    }
    if ((S->Flags&CR_UFS_POWER_VCC_LOW_VOLTAGE) && Role!=CrUfsPowerVcc)
      return EFI_COMPROMISED_DATA;
    if (Used[Index]) { Lo=MAX(Lo,Low[Index]); Hi=MIN(Hi,High[Index]); }
    if (Lo>Hi) return EFI_UNSUPPORTED;
    Used[Index]=TRUE; Low[Index]=Lo; High[Index]=Hi;
    if (P->RetainedRoles & (1U<<Role)) Retained[Index]=TRUE;
  }
  /* Target rule order preserves PHY-before-device rail acquisition. A shared
     physical rail receives one vote satisfying every consumer in the tree. */
  for (UINT32 I=0; I<P->RuleCount; ++I) {
    CONST CR_TARGET_UFS_POWER_RULE *R=&P->Rules[I];
    UINT32 Uv, Source;
    if (!Used[I]) continue;
    if (Init[I]>=Low[I] && Init[I]<=High[I]) { Uv=Init[I]; Source=1; }
    else if (Low[I]==High[I]) { Uv=Low[I]; Source=1; }
    else { Uv=R->PreferredUv; Source=2; }
    if (Uv<Low[I] || Uv>High[I] || !Representable(R,Uv)) return EFI_UNSUPPORTED;
    Out.Rails[Out.Count]=(CR_TARGET_UFS_RAIL){R->Name,Uv/1000,R->Mode};
    if (Retained[I]) Out.RetainedRailMask |= 1U<<Out.Count;
    Out.Sources[Out.Count++]=Source;
  }
  if (!Out.Count) return EFI_UNSUPPORTED;
  *Result=Out;
  return EFI_SUCCESS;
}
