/* SPDX-License-Identifier: MIT */
#include <Library/SerialResourceLib.h>
STATIC CR_STATUS Exact(CR_STATUS S){return S==CR_SUCCESS?S:CR_ERROR(S)?S:CR_NOT_READY;}
CR_STATUS CrSerialResourcesStart(CR_SERIAL_RESOURCES *R,CONST CR_SERIAL_RESOURCE_IO *Io,VOID *Owner,CONST UINT32 Counts[CR_SERIAL_RESOURCE_GROUPS]){
  if(!R||!Io||!Owner||!Counts||!Io->Context||!Io->Validate||!Io->Reserve||!Io->Begin||!Io->Apply||!Io->Release||!Io->End||!Io->Check)return CR_INVALID_PARAMETER;
  if(R->Owner||R->Quarantined)return CR_BUSY;
  for(UINT32 G=0;G<CR_SERIAL_RESOURCE_GROUPS;++G){
    if(!Counts[G]||Counts[G]>4)return CR_INVALID_PARAMETER;
    for(UINT32 I=0;I<Counts[G];++I){CR_STATUS S=Exact(Io->Validate(Io->Context,G,I));if(S!=CR_SUCCESS)return S;}
  }
  /* Pure validation above cannot leave hardware owned. From here each failure
     retains this exact immutable frame, including a partially committed Begin. */
  R->Io.Context=Io->Context;R->Io.Validate=Io->Validate;R->Io.Reserve=Io->Reserve;
  R->Io.Begin=Io->Begin;R->Io.Apply=Io->Apply;R->Io.Release=Io->Release;
  R->Io.End=Io->End;R->Io.Check=Io->Check;R->Owner=Owner;
  for(UINT32 G=0;G<CR_SERIAL_RESOURCE_GROUPS;++G)R->Counts[G]=Counts[G];
  BOOLEAN Owned=FALSE;CR_STATUS S=Exact(Io->Begin(Io->Context,Owner,&Owned));R->Lease=Owned;
  if(S!=CR_SUCCESS)return S;
  if(!Owned||!Io->Check(Io->Context,Owner))return CR_NOT_READY;
  for(UINT32 G=0;G<CR_SERIAL_RESOURCE_GROUPS;++G){
    R->Groups[G]=TRUE;
    for(UINT32 I=0;I<Counts[G];++I){S=Exact(Io->Reserve(Io->Context,Owner,G,I));if(S!=CR_SUCCESS)return S;}
  }
  /* Clock and ICB access precede pad programming; all owner namespaces remain
     separate, so cleanup can never withdraw a compatible foreign vote. */
  for(UINT32 G=0;G<CR_SERIAL_RESOURCE_GROUPS;++G)
    for(UINT32 I=0;I<Counts[G];++I){
      if(!Io->Check(Io->Context,Owner))return CR_NOT_READY;
      S=Exact(Io->Apply(Io->Context,Owner,G,I));if(S!=CR_SUCCESS)return S;
    }
  if(!Io->Check(Io->Context,Owner))return CR_NOT_READY;
  R->Ready=TRUE;return CR_SUCCESS;
}
CR_STATUS CrSerialResourcesRetire(CR_SERIAL_RESOURCES *R){
  if(!R)return CR_INVALID_PARAMETER;
  if(!R->Owner)return CR_SUCCESS;
  R->Ready=FALSE;R->Retiring=TRUE;
  if(R->Quarantined)return CR_NOT_READY;
  for(UINT32 N=CR_SERIAL_RESOURCE_GROUPS;N!=0;--N){UINT32 G=N-1;
    if(R->Groups[G]){CR_STATUS S=Exact(R->Io.Release(R->Io.Context,R->Owner,G));if(S!=CR_SUCCESS)return S;R->Groups[G]=FALSE;}
  }
  if(R->Lease){CR_STATUS S=Exact(R->Io.End(R->Io.Context,R->Owner));if(S!=CR_SUCCESS)return S;R->Lease=FALSE;}
  R->Owner=NULL;R->Retiring=FALSE;return CR_SUCCESS;
}
VOID CrSerialResourcesQuarantine(CR_SERIAL_RESOURCES *R){if(R){R->Quarantined=TRUE;R->Ready=FALSE;}}
BOOLEAN CrSerialResourcesReady(CONST CR_SERIAL_RESOURCES *R){return R&&R->Owner&&R->Lease&&R->Ready&&!R->Retiring&&!R->Quarantined&&R->Io.Check(R->Io.Context,R->Owner);}
