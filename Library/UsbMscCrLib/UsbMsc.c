/* Portable RAM/block-disk SCSI / Bulk-Only Transport. SPDX-License-Identifier: MIT */
#include <Library/UsbMscCrLib.h>

#define S_CBW 1U
#define S_DATA 2U
#define S_ZLP 3U
#define S_HALT_IN 4U
#define S_HALT_OUT 5U
#define S_WAIT_IN 6U
#define S_WAIT_OUT 7U
#define S_CSW 8U
#define S_RECOVERY 9U
#define S_DISCARD 10U
#define D_NONE 0U
#define D_IN 1U
#define D_OUT 2U
#define PART_START 2048U
#define PART_SECTORS 30720U
#define FAT_SECTORS 60U
#define ROOT_SECTORS 32U
#define DATA_START (PART_START + 1U + 2U * FAT_SECTORS + ROOT_SECTORS)

STATIC VOID Zero(VOID *P,UINTN N){UINT8 *B=P;while(N--)*B++=0;}
STATIC VOID Copy(VOID *D,CONST VOID *S,UINTN N){UINT8 *A=D;CONST UINT8 *B=S;while(N--)*A++=*B++;}
STATIC UINT32 Le32(CONST UINT8 *P){return (UINT32)P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
STATIC UINT32 Be16(CONST UINT8 *P){return ((UINT32)P[0]<<8)|P[1];}
STATIC UINT32 Be32(CONST UINT8 *P){return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];}
STATIC UINT64 Be64(CONST UINT8 *P){return ((UINT64)Be32(P)<<32)|Be32(P+4);}
STATIC VOID PutLe16(UINT8 *P,UINT32 V){P[0]=(UINT8)V;P[1]=(UINT8)(V>>8);}
STATIC VOID PutLe32(UINT8 *P,UINT32 V){for(UINT32 I=0;I<4;++I)P[I]=(UINT8)(V>>(I*8));}
STATIC VOID PutBe32(UINT8 *P,UINT32 V){for(UINT32 I=0;I<4;++I)P[I]=(UINT8)(V>>((3-I)*8));}
STATIC VOID PutBe64(UINT8 *P,UINT64 V){PutBe32(P,(UINT32)(V>>32));PutBe32(P+4,(UINT32)V);}
STATIC BOOLEAN Range(CONST VOID *P,UINTN N){return P&&N&&(UINTN)P<=~(UINTN)0-N;}
STATIC BOOLEAN Separate(CONST VOID *P,UINTN N,CONST VOID *Q,UINTN M){return Range(P,N)&&Range(Q,M)&&((UINTN)P+N<=(UINTN)Q||(UINTN)Q+M<=(UINTN)P);}
STATIC BOOLEAN Valid(CONST CR_USB_MSC *C){
  if(!C||!C->Initialized||!C->LunCount||C->LunCount>CR_USB_MSC_MAX_LUNS)return FALSE;
  if(!C->BlockBackend)return C->LunCount==1&&C->Disk&&C->DiskBytes==CR_USB_MSC_DISK_BYTES;
  for(UINT8 I=0;I<C->LunCount;++I)if(!C->Luns[I].Backend.Read)return FALSE;
  return TRUE;
}
STATIC CR_USB_MSC_LUN *Lun(CR_USB_MSC *C){
  return C->ActiveLun<C->LunCount?&C->Luns[C->ActiveLun]:&C->UnsupportedLun;
}
STATIC CONST CR_USB_MSC_BLOCK_BACKEND *Backend(CONST CR_USB_MSC *C){
  return &C->Luns[C->ActiveLun].Backend; /* Only admitted, supported LUN paths. */
}
STATIC UINT32 BlockBytes(CONST CR_USB_MSC *C){return C->BlockBackend?Backend(C)->BlockSize:512U;}
STATIC UINT64 LastLba(CONST CR_USB_MSC *C){return C->BlockBackend?Backend(C)->LastBlock:C->DiskBytes/512U-1U;}
STATIC VOID Sense(CR_USB_MSC *C,UINT8 Key,UINT8 Asc,UINT8 Ascq){Lun(C)->SenseKey=Key;Lun(C)->SenseAsc=Asc;Lun(C)->SenseAscq=Ascq;C->Status=1;}
STATIC VOID BadField(CR_USB_MSC *C){Sense(C,5,0x24,0);}
STATIC VOID Reply(CR_USB_MSC *C,UINT32 Bytes,UINT32 Allocation){C->DeviceBytes=Bytes<Allocation?Bytes:Allocation;C->DeviceDirection=C->DeviceBytes?D_IN:D_NONE;}
STATIC BOOLEAN Length(CR_USB_MSC *C,UINT8 N){if(C->Cbw[14]!=N){BadField(C);return FALSE;}return TRUE;}
STATIC VOID Format(UINT8 *D){
  STATIC CONST CHAR8 Text[]="Crane USB device-mode RAM disk test\r\n\r\nThis writable 16 MiB disk exists only in RAM.\r\nIt does not expose the HDK UFS, SD card, or NVMe.\r\nFiles are lost when the application exits or the board reboots.\r\nUse this volume to test USB enumeration and file transfers.\r\n";
  Zero(D,CR_USB_MSC_DISK_BYTES);
  UINT8 *P=D+446;P[1]=P[5]=0xfe;P[2]=P[3]=P[6]=P[7]=0xff;P[4]=6;
  PutLe32(P+8,PART_START);PutLe32(P+12,PART_SECTORS);D[510]=0x55;D[511]=0xaa;
  UINT8 *B=D+PART_START*512U;B[0]=0xeb;B[1]=0x3c;B[2]=0x90;Copy(B+3,"CRANEMSC",8);
  PutLe16(B+11,512);B[13]=2;PutLe16(B+14,1);B[16]=2;PutLe16(B+17,512);
  PutLe16(B+19,PART_SECTORS);B[21]=0xf8;PutLe16(B+22,FAT_SECTORS);
  PutLe16(B+24,32);PutLe16(B+26,64);PutLe32(B+28,PART_START);B[36]=0x80;B[38]=0x29;
  PutLe32(B+39,0x43524d53);Copy(B+43,"CRANE TEST ",11);Copy(B+54,"FAT16   ",8);B[510]=0x55;B[511]=0xaa;
  for(UINT32 I=0;I<2;++I){UINT8 *F=D+(PART_START+1U+I*FAT_SECTORS)*512U;PutLe16(F,0xfff8);PutLe16(F+2,0xffff);PutLe16(F+4,0xffff);}
  UINT8 *R=D+(PART_START+1U+2U*FAT_SECTORS)*512U;Copy(R,"CRANE TEST ",11);R[11]=8;
  R+=32;Copy(R,"README  TXT",11);R[11]=0x20;PutLe16(R+16,0x5d41);PutLe16(R+24,0x5d41);PutLe16(R+26,2);PutLe32(R+28,sizeof(Text)-1);
  Copy(D+DATA_START*512U,Text,sizeof(Text)-1);
}
STATIC VOID MediaFailure(CR_USB_MSC *C,CR_STATUS S,BOOLEAN Write){
  UINT8 Status=C->Status;
  if(S==CR_NOT_FOUND||S==CR_ABORT)Lun(C)->MediaError=S;
  if(S==CR_NOT_FOUND)Sense(C,2,0x3a,0);
  else if(S==CR_ABORT)Sense(C,6,0x28,0);
  else if(S==CR_BUSY)Sense(C,7,0x27,0);
  else if(S==CR_NOT_READY)Sense(C,2,4,1);
  else Sense(C,3,Write?0x0c:0x11,Write?2:0);
  /* A media error cannot discharge an already selected BOT phase error. */
  if(Status==2)C->Status=2;
}
STATIC BOOLEAN Media(CR_USB_MSC *C){
  if(Lun(C)->MediaError!=CR_SUCCESS){MediaFailure(C,Lun(C)->MediaError,FALSE);return FALSE;}
  if(Lun(C)->Ejected){Sense(C,2,0x3a,0);return FALSE;}return TRUE;
}
STATIC BOOLEAN BlockRange(CR_USB_MSC *C,UINT64 Lba,UINT32 Count){
  UINT64 Last=LastLba(C);
  if(Count&&(Lba>Last||(UINT64)(Count-1U)>Last-Lba)){Sense(C,5,0x21,0);return FALSE;}
  return TRUE;
}
STATIC CR_STATUS Flush(CR_USB_MSC *C){
  return C->BlockBackend&&Backend(C)->Flush?Backend(C)->Flush(Backend(C)->Context):CR_SUCCESS;
}
STATIC VOID CheckMedia(CR_USB_MSC *C){
  if(C->BlockBackend&&C->CheckRequested&&Backend(C)->Check){
    CR_STATUS S=Backend(C)->Check(Backend(C)->Context);
    if(S!=CR_SUCCESS)MediaFailure(C,S,FALSE);
  }
}
STATIC VOID Blocks(CR_USB_MSC *C,UINT64 Lba,UINT32 Count,BOOLEAN In){
  if(!Media(C))return;
  if(!BlockRange(C,Lba,Count))return;
  UINT32 Size=BlockBytes(C);
  if(Count>0xffffffffU/Size){BadField(C);return;}
  if(!Count)return;
  if(!In&&C->BlockBackend&&Backend(C)->ReadOnly){Sense(C,7,0x27,0);return;}
  C->MediaLba=Lba;C->DiskOffset=C->BlockBackend?0:(UINT32)Lba*Size;
  C->DeviceBytes=Count*Size;C->DeviceDirection=In?D_IN:D_OUT;C->DataOnDisk=TRUE;
}
STATIC VOID Scsi(CR_USB_MSC *C){
  CONST UINT8 *B=C->Cbw+15;
  Zero(C->Reply,sizeof(C->Reply));C->Status=0;C->DeviceBytes=0;C->DeviceDirection=D_NONE;C->DataOnDisk=FALSE;C->DiskOffset=0;C->SyncRequested=C->CheckRequested=FALSE;
  if(C->ActiveLun>=C->LunCount){
    Sense(C,5,0x25,0);
    /* REQUEST SENSE is the diagnostic path for an unsupported low-nibble LUN. */
    if(B[0]!=0x03&&(B[0]!=0x12||B[1]||B[2]))return;
    C->Status=0;
  }
  if(B[C->Cbw[14]-1]){BadField(C);return;}
  switch(B[0]){
  case 0x00: /* TEST UNIT READY */
    if(!Length(C,6))break;if(B[1]||B[2]||B[3]||B[4]){BadField(C);break;}
    if(Media(C))C->CheckRequested=TRUE;break;
  case 0x03: /* REQUEST SENSE, fixed format */
    if(!Length(C,6))break;if(B[1]){BadField(C);break;}
    C->Reply[0]=0x70;C->Reply[2]=Lun(C)->SenseKey;C->Reply[7]=10;C->Reply[12]=Lun(C)->SenseAsc;C->Reply[13]=Lun(C)->SenseAscq;
    Reply(C,18,B[4]);Lun(C)->SenseKey=Lun(C)->SenseAsc=Lun(C)->SenseAscq=0;break;
  case 0x12: /* INQUIRY */
    if(!Length(C,6))break;if(B[1]&~1U){BadField(C);break;}
    if(B[1]&1U){
      C->Reply[1]=B[2];
      if(B[2]==0){C->Reply[3]=2;C->Reply[4]=0;C->Reply[5]=0x80;Reply(C,6,Be16(B+3));}
      else if(B[2]==0x80){C->Reply[3]=16;Copy(C->Reply+4,"0000000000000001",16);C->Reply[19]=(UINT8)('1'+C->ActiveLun);Reply(C,20,Be16(B+3));}
      else BadField(C);
    }else{
      if(B[2]){BadField(C);break;}
      if(C->ActiveLun>=C->LunCount){C->Reply[0]=0x7f;C->Reply[4]=31;Reply(C,36,Be16(B+3));break;}
      C->Reply[1]=0x80;C->Reply[2]=5;C->Reply[3]=2;C->Reply[4]=31;
      Copy(C->Reply+8,"CRANE   ",8);Copy(C->Reply+16,C->BlockBackend?"BLOCK DISK TEST ":"RAM DISK TEST   ",16);Copy(C->Reply+32,"0001",4);Reply(C,36,Be16(B+3));
    }break;
  case 0x1a: case 0x5a: { /* MODE SENSE: cache page, no block descriptors */
    BOOLEAN Ten=B[0]==0x5a;if(!Length(C,Ten?10:6))break;
    if((B[1]&~8U)||(B[2]&0xc0)||B[3]||((B[2]&0x3f)!=0x3f&&(B[2]&0x3f)!=8)){BadField(C);break;}
    UINT32 H=Ten?8:4;C->Reply[H]=8;C->Reply[H+1]=18; /* WCE=0; block writes flush before success. */
    C->Reply[Ten?3:2]=C->BlockBackend&&Backend(C)->ReadOnly?0x80:0;
    if(Ten){C->Reply[1]=(UINT8)(H+20-2);Reply(C,H+20,Be16(B+7));}
    else{C->Reply[0]=(UINT8)(H+20-1);Reply(C,H+20,B[4]);}break;
  }
  case 0x1b: /* START STOP UNIT */
    if(!Length(C,6))break;if(B[1]&~1U||B[2]||B[3]||(B[4]&~3U)){BadField(C);break;}
    if((B[4]&3U)==2U&&Lun(C)->PreventRemoval){Sense(C,5,0x53,2);break;}
    if(B[4]&2U)Lun(C)->Ejected=(B[4]&1U)==0;else if(B[4]&1U)Lun(C)->Ejected=FALSE;break;
  case 0x1e: /* PREVENT ALLOW MEDIUM REMOVAL */
    if(!Length(C,6))break;if(B[1]||B[2]||B[3]||(B[4]&~1U)){BadField(C);break;}Lun(C)->PreventRemoval=(B[4]&1U)!=0;break;
  case 0x23: /* READ FORMAT CAPACITIES */
    if(!Length(C,10)||!Media(C))break;C->Reply[3]=8;
    PutBe32(C->Reply+4,LastLba(C)>=0xffffffffU?0xffffffffU:(UINT32)(LastLba(C)+1));
    PutBe32(C->Reply+8,BlockBytes(C));C->Reply[8]=2;Reply(C,12,Be16(B+7));break;
  case 0x25: /* READ CAPACITY(10) */
    if(!Length(C,10)||!Media(C))break;if(B[1]||B[6]||B[7]||(B[8]&~1U)||(!(B[8]&1U)&&Be32(B+2))){BadField(C);break;}
    C->CheckRequested=TRUE;
    PutBe32(C->Reply,LastLba(C)>0xffffffffU?0xffffffffU:(UINT32)LastLba(C));PutBe32(C->Reply+4,BlockBytes(C));Reply(C,8,8);break;
  case 0x9e: /* READ CAPACITY(16) */
    if(!Length(C,16)||!Media(C))break;if(B[1]!=0x10||B[14]||Be64(B+2)){BadField(C);break;}
    C->CheckRequested=TRUE;
    PutBe64(C->Reply,LastLba(C));PutBe32(C->Reply+8,BlockBytes(C));Reply(C,32,Be32(B+10));break;
  case 0x28: case 0x2a: /* READ/WRITE(10), allow DPO/FUA */
    if(!Length(C,10))break;if((B[1]&~0x18U)||B[6]){BadField(C);break;}
    Blocks(C,Be32(B+2),Be16(B+7),B[0]==0x28);break;
  case 0xa8: case 0xaa: /* READ/WRITE(12) */
    if(!Length(C,12))break;if((B[1]&~0x18U)||B[10]){BadField(C);break;}
    Blocks(C,Be32(B+2),Be32(B+6),B[0]==0xa8);break;
  case 0x88: case 0x8a: /* READ/WRITE(16) */
    if(!Length(C,16))break;if((B[1]&~0x18U)||B[14]){BadField(C);break;}
    Blocks(C,Be64(B+2),Be32(B+10),B[0]==0x88);break;
  case 0x2f: /* VERIFY(10), BYTCHK=0 */
    if(!Length(C,10)||!Media(C))break;if(B[1]||B[6]){BadField(C);break;}
    if(C->BlockBackend){Sense(C,5,0x20,0);break;}
    BlockRange(C,Be32(B+2),Be16(B+7));break;
  case 0x35: { /* SYNCHRONIZE CACHE(10), synchronous whole selected medium */
    if(!Length(C,10)||!Media(C))break;if(B[1]||B[6]){BadField(C);break;}
    UINT32 Count=Be16(B+7); /* Zero means through the end, not an empty range. */
    if(!BlockRange(C,Be32(B+2),Count?Count:1U))break;C->SyncRequested=TRUE;break;
  }
  case 0x91: { /* SYNCHRONIZE CACHE(16) */
    if(!Length(C,16)||!Media(C))break;if(B[1]||B[14]){BadField(C);break;}
    UINT32 Count=Be32(B+10);
    if(!BlockRange(C,Be64(B+2),Count?Count:1U))break;C->SyncRequested=TRUE;break;
  }
  case 0xa0: { /* REPORT LUNS: peripheral addressing, no well-known LUNs. */
    if(!Length(C,12))break;
    if(B[1]||B[2]>2||B[3]||B[4]||B[5]||B[10]){BadField(C);break;}
    UINT8 Count=B[2]==1?0:C->LunCount;
    PutBe32(C->Reply,(UINT32)Count*8U);
    for(UINT8 I=0;I<Count;++I)C->Reply[8U+(UINT32)I*8U+1U]=I;
    Reply(C,8U+(UINT32)Count*8U,Be32(B+6));break;
  }
  default: Sense(C,5,0x20,0);break;
  }
}
STATIC VOID Recovery(CR_USB_MSC *C){C->State=S_RECOVERY;C->ResetRequired=TRUE;C->ResetSeen=FALSE;C->ClearIn=C->ClearOut=FALSE;C->Issued=FALSE;}
STATIC VOID Csw(CR_USB_MSC *C){
  PutLe32(C->Csw,0x53425355);Copy(C->Csw+4,C->Cbw+4,4);PutLe32(C->Csw+8,C->Residue);C->Csw[12]=C->Status;C->State=S_CSW;
}
STATIC VOID DataFinished(CR_USB_MSC *C){
  if(C->NeedZlp){C->NeedZlp=FALSE;C->State=S_ZLP;return;}
  if(C->AfterStall){C->State=C->AfterStall;C->AfterStall=0;if(C->State==S_DISCARD)C->Remaining=C->Residue;return;}
  Csw(C);
}
STATIC VOID Command(CR_USB_MSC *C,UINT32 Bytes){
  if(Bytes!=31||Le32(C->Cbw)!=0x43425355||(C->Cbw[12]&0x7f)||(C->Cbw[13]&0xf0)||!C->Cbw[14]||C->Cbw[14]>16){Recovery(C);return;}
  C->HostBytes=Le32(C->Cbw+8);C->Residue=C->HostBytes;C->DoneBytes=0;C->Remaining=0;C->AfterStall=0;C->NeedZlp=FALSE;
  C->ActiveLun=C->Cbw[13];
  Scsi(C);
  BOOLEAN In=(C->Cbw[12]&0x80)!=0;UINT32 H=C->HostBytes,D=C->DeviceBytes;
  /* Valid no-data commands execute in cases 1,4,9, including excess host
     data. The halt/discard below retains that entire host-byte residue. */
  if(!D&&!C->Status){
    CheckMedia(C);
    if(C->SyncRequested&&!C->Status){CR_STATUS S=Flush(C);if(S!=CR_SUCCESS)MediaFailure(C,S,TRUE);}
  }
  if(!H){
    if(D)C->Status=2;
    Csw(C);return;
  } /* cases 1,2,3 */
  if(!D){C->State=In?S_HALT_IN:S_DISCARD;C->Remaining=H;return;} /* cases 4,9 */
  if((In&&C->DeviceDirection!=D_IN)||(!In&&C->DeviceDirection!=D_OUT)){
    C->Status=2;C->State=In?S_HALT_IN:S_DISCARD;C->Remaining=H;return; /* cases 8,10 */
  }
  if(H<D){C->Status=2;if(!In){C->State=S_DISCARD;C->Remaining=H;return;}} /* case 13: no partial writes */
  if(!C->Status){CheckMedia(C);if(C->Status){C->State=In?S_HALT_IN:S_DISCARD;C->Remaining=H;return;}}
  C->Remaining=H<D?H:D;
  if(H>D){C->AfterStall=In?S_HALT_IN:S_DISCARD;C->NeedZlp=In&&(D%C->MaxPacket)==0;} /* 5,11 */
  C->State=S_DATA; /* cases 5,6,7,11,12 */
}
CR_STATUS CrUsbMscInit(CR_USB_MSC *C,VOID *Disk,UINTN Bytes){
  if(Bytes!=CR_USB_MSC_DISK_BYTES||!Separate(C,sizeof(*C),Disk,Bytes))return CR_INVALID_PARAMETER;
  for(UINTN I=0;I<sizeof(*C);++I)if(((UINT8*)C)[I])return CR_INVALID_PARAMETER;
  C->Disk=Disk;C->DiskBytes=(UINT32)Bytes;C->LunCount=1;C->State=S_CBW;C->MaxPacket=64;Format(Disk);C->Initialized=TRUE;return CR_SUCCESS;
}
CR_STATUS CrUsbMscInitBlock(CR_USB_MSC *C,CONST CR_USB_MSC_BLOCK_BACKEND *B){
  return CrUsbMscInitBlocks(C,B,1);
}
CR_STATUS CrUsbMscInitBlocks(CR_USB_MSC *C,CONST CR_USB_MSC_BLOCK_BACKEND *B,UINT8 Count){
  if(!Count||Count>CR_USB_MSC_MAX_LUNS||!Separate(C,sizeof(*C),B,(UINTN)Count*sizeof(*B)))return CR_INVALID_PARAMETER;
  for(UINT8 I=0;I<Count;++I){
    if(B[I].BlockSize<512||B[I].BlockSize>CR_USB_MSC_TRANSFER_MAX||
       (B[I].BlockSize&(B[I].BlockSize-1U))||B[I].ReadOnly>1||!B[I].Read||
       (!B[I].ReadOnly&&(!B[I].Write||!B[I].Flush)))return CR_INVALID_PARAMETER;
  }
  for(UINTN I=0;I<sizeof(*C);++I)if(((UINT8*)C)[I])return CR_INVALID_PARAMETER;
  for(UINT8 I=0;I<Count;++I)C->Luns[I].Backend=B[I];
  C->LunCount=Count;C->BlockBackend=TRUE;C->State=S_CBW;C->MaxPacket=64;C->Initialized=TRUE;return CR_SUCCESS;
}
CR_STATUS CrUsbMscGetMaxLun(CONST CR_USB_MSC *C,UINT8 *Max){
  if(!Valid(C)||!Separate(C,sizeof(*C),Max,sizeof(*Max))||
     (!C->BlockBackend&&!Separate(C->Disk,C->DiskBytes,Max,sizeof(*Max))))return CR_INVALID_PARAMETER;
  *Max=(UINT8)(C->LunCount-1U);return CR_SUCCESS;
}
CR_STATUS CrUsbMscSetMaxPacket(CR_USB_MSC *C,UINT16 Packet){
  if(!Valid(C)||(Packet!=64&&Packet!=512&&Packet!=1024))return CR_INVALID_PARAMETER;
  if(C->Issued||C->State!=S_CBW)return CR_BUSY;C->MaxPacket=Packet;return CR_SUCCESS;
}
CR_STATUS CrUsbMscNext(CR_USB_MSC *C,CR_USB_MSC_ACTION *A){
  if(!Valid(C)||!Separate(C,sizeof(*C),A,sizeof(*A))||
     (!C->BlockBackend&&!Separate(C->Disk,C->DiskBytes,A,sizeof(*A))))return CR_INVALID_PARAMETER;
  if(C->Issued)return CR_BUSY;
  A->Type=CrUsbMscNone;A->Buffer=NULL;A->Length=0;
  if(C->State==S_RECOVERY){
    if(C->ResetSeen)return CR_SUCCESS;
    if(!C->HaltIn)A->Type=CrUsbMscStallIn;else if(!C->HaltOut)A->Type=CrUsbMscStallOut;else return CR_SUCCESS;
  }else if(C->State==S_HALT_IN)A->Type=CrUsbMscStallIn;
  else if(C->State==S_HALT_OUT)A->Type=CrUsbMscStallOut;
  else if(C->State==S_WAIT_IN||C->State==S_WAIT_OUT)return CR_SUCCESS;
  else if(C->State==S_CBW){if(C->HaltOut)return CR_SUCCESS;A->Type=CrUsbMscBulkOut;A->Buffer=C->Cbw;A->Length=31;}
  else if(C->State==S_CSW){if(C->HaltIn)return CR_SUCCESS;A->Type=CrUsbMscBulkIn;A->Buffer=C->Csw;A->Length=13;}
  else if(C->State==S_ZLP){A->Type=CrUsbMscBulkIn;A->Buffer=C->Reply;}
  else if(C->State==S_DISCARD){A->Type=CrUsbMscBulkOut;A->Buffer=C->Discard;
    A->Length=C->Remaining<CR_USB_MSC_TRANSFER_MAX?C->Remaining:CR_USB_MSC_TRANSFER_MAX;}
  else if(C->State==S_DATA){
    A->Type=C->DeviceDirection==D_IN?CrUsbMscBulkIn:CrUsbMscBulkOut;
    A->Length=C->Remaining<CR_USB_MSC_TRANSFER_MAX?C->Remaining:CR_USB_MSC_TRANSFER_MAX;
    if(C->DataOnDisk&&C->BlockBackend){
      A->Buffer=C->Discard;
      if(C->DeviceDirection==D_IN){
        UINT32 Size=BlockBytes(C),Count=(A->Length+Size-1U)/Size;
        CR_STATUS S=Backend(C)->Read(Backend(C)->Context,C->MediaLba+C->DoneBytes/Size,Count,C->Discard);
        if(S!=CR_SUCCESS){MediaFailure(C,S,FALSE);C->State=S_HALT_IN;C->AfterStall=0;C->NeedZlp=FALSE;return CrUsbMscNext(C,A);}
      }
    }else A->Buffer=C->DataOnDisk?C->Disk+C->DiskOffset+C->DoneBytes:C->Reply+C->DoneBytes;
  }else return CR_COMPROMISED_DATA;
  C->Issued=TRUE;C->IssuedType=A->Type;C->IssuedLength=A->Length;return CR_SUCCESS;
}
CR_STATUS CrUsbMscComplete(CR_USB_MSC *C,BOOLEAN Success,UINT32 Bytes){
  if(!Valid(C)||!C->Issued)return CR_INVALID_PARAMETER;
  UINT32 Type=C->IssuedType,Want=C->IssuedLength;C->Issued=FALSE;C->IssuedLength=C->IssuedType=0;
  if(!Success||Bytes>Want){Recovery(C);return Success?CR_BAD_BUFFER_SIZE:CR_DEVICE_ERROR;}
  if(Type==CrUsbMscStallIn||Type==CrUsbMscStallOut){
    if(Bytes){Recovery(C);return CR_BAD_BUFFER_SIZE;}
    if(Type==CrUsbMscStallIn)C->HaltIn=TRUE;else C->HaltOut=TRUE;
    if(C->State!=S_RECOVERY)C->State=Type==CrUsbMscStallIn?S_WAIT_IN:S_WAIT_OUT;return CR_SUCCESS;
  }
  if(C->State==S_CBW){Command(C,Bytes);return CR_SUCCESS;}
  if(Bytes!=Want){Recovery(C);return CR_BAD_BUFFER_SIZE;}
  if(C->State==S_DISCARD){C->Remaining-=Bytes;if(!C->Remaining)Csw(C);return CR_SUCCESS;}
  if(C->State==S_DATA){
    if(C->DataOnDisk&&C->BlockBackend&&C->DeviceDirection==D_OUT){
      UINT32 Size=BlockBytes(C);
      CR_STATUS S=Backend(C)->Write(Backend(C)->Context,C->MediaLba+C->DoneBytes/Size,Bytes/Size,C->Discard);
      if(S==CR_SUCCESS)S=Flush(C);
      if(S!=CR_SUCCESS){
        MediaFailure(C,S,TRUE);C->Remaining=C->HostBytes-C->DoneBytes-Bytes;
        C->AfterStall=0;C->NeedZlp=FALSE;if(C->Remaining)C->State=S_DISCARD;else Csw(C);return CR_SUCCESS;
      }
    }
    C->DoneBytes+=Bytes;C->Remaining-=Bytes;C->Residue-=Bytes;if(!C->Remaining)DataFinished(C);return CR_SUCCESS;
  }
  if(C->State==S_ZLP){DataFinished(C);return CR_SUCCESS;}
  if(C->State==S_CSW){if(C->Status==2)Recovery(C);else C->State=S_CBW;return CR_SUCCESS;}
  Recovery(C);return CR_COMPROMISED_DATA;
}
STATIC VOID ResetLuns(CR_USB_MSC *C,BOOLEAN Bus){
  for(UINT8 I=0;I<C->LunCount;++I){
    C->Luns[I].PreventRemoval=FALSE;
    if(Bus){
      C->Luns[I].SenseKey=C->Luns[I].SenseAsc=C->Luns[I].SenseAscq=0;
      C->Luns[I].Ejected=FALSE;
    }
  }
  if(Bus)Zero(&C->UnsupportedLun,sizeof(C->UnsupportedLun));
  C->ActiveLun=0;
}
CR_STATUS CrUsbMscReset(CR_USB_MSC *C){
  if(!Valid(C))return CR_INVALID_PARAMETER;
  /* Drain is an adapter obligation; reset cancels the software-issued token. */
  C->Issued=FALSE;C->IssuedLength=C->IssuedType=0;C->State=S_RECOVERY;C->ResetRequired=TRUE;C->ResetSeen=TRUE;
  C->ClearIn=C->ClearOut=FALSE;C->NeedZlp=FALSE;C->AfterStall=0;ResetLuns(C,FALSE);return CR_SUCCESS;
}
CR_STATUS CrUsbMscBusReset(CR_USB_MSC *C){
  if(!Valid(C))return CR_INVALID_PARAMETER;
  C->Issued=FALSE;C->IssuedLength=C->IssuedType=0;C->State=S_CBW;C->HaltIn=C->HaltOut=FALSE;
  C->ResetRequired=C->ResetSeen=C->ClearIn=C->ClearOut=FALSE;C->NeedZlp=FALSE;C->AfterStall=0;
  ResetLuns(C,TRUE);return CR_SUCCESS;
}
BOOLEAN CrUsbMscClearHalt(CR_USB_MSC *C,BOOLEAN In){
  if(!Valid(C)||C->Issued||(C->ResetRequired&&!C->ResetSeen))return FALSE;
  if(In){C->HaltIn=FALSE;C->ClearIn=TRUE;}else{C->HaltOut=FALSE;C->ClearOut=TRUE;}
  if(C->ResetRequired){if(C->ClearIn&&C->ClearOut){C->ResetRequired=C->ResetSeen=FALSE;C->State=S_CBW;}}
  else if((In&&C->State==S_WAIT_IN)||(!In&&C->State==S_WAIT_OUT))Csw(C);
  return TRUE;
}
