/* Actual DAL-protocol consumer and BDS launch policy, with services mocked.
 * SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <Library/AndroidBootCrLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Protocol/LoadedImage.h>

#include "../Library/ChargerBootCrLib/ChargerBootCrLib.c"

EFI_GUID gEfiCrFdtProtocolGuid, gChargerCrFileGuid, gEfiLoadedImageProtocolGuid;
EFI_HANDLE gImageHandle = (EFI_HANDLE)0x1234;
STATIC EFI_BOOT_SERVICES Services;
STATIC EFI_RUNTIME_SERVICES Runtime;
EFI_BOOT_SERVICES *gBS = &Services;
EFI_RUNTIME_SERVICES *gRT = &Runtime;
STATIC CR_ANDROID_BOOT_INFO BootInfo;
STATIC EFI_CR_FDT_PROTOCOL Fdt;
STATIC BOOLEAN HaveProtocol, FailPath, NoImage;
STATIC EFI_STATUS LocateResult, BootResult;
STATIC UINTN Loads, Starts, Unloads, Resets;
STATIC EFI_STATUS LoadResult, StartResult, ParentResult;
STATIC jmp_buf ResetJump;
STATIC EFI_DEVICE_PATH_PROTOCOL ParentPath = {HARDWARE_DEVICE_PATH, 1, {4,0}};
STATIC EFI_LOADED_IMAGE_PROTOCOL ParentImage;

BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN Level) { (VOID)Level; return FALSE; }
VOID EFIAPI DebugPrint (UINTN Level, CONST CHAR8 *Format, ...) { (VOID)Level; (VOID)Format; }
INTN EFIAPI AsciiStrCmp (CONST CHAR8 *A, CONST CHAR8 *B) { return strcmp (A,B); }
VOID EFIAPI CpuDeadLoop (VOID) { abort (); }
STATIC EFI_STATUS EFIAPI GetBootInfo (EFI_CR_FDT_PROTOCOL *This, CR_ANDROID_BOOT_INFO *Info)
{ assert (This == &Fdt); *Info = BootInfo; return BootResult; }
STATIC EFI_STATUS EFIAPI LocateProtocol (EFI_GUID *Guid, VOID *Registration, VOID **Out)
{ assert (Guid == &gEfiCrFdtProtocolGuid && Registration == NULL); *Out = HaveProtocol ? &Fdt : NULL; return LocateResult; }
VOID EFIAPI FreePool (VOID *Buffer) { free (Buffer); }
VOID EFIAPI EfiInitializeFwVolDevicepathNode (MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *Node,
                                             CONST EFI_GUID *Guid)
{
  assert (Guid == &gChargerCrFileGuid);
  memset (Node,0,sizeof (*Node));
  Node->Header.Type = MEDIA_DEVICE_PATH;
  Node->Header.SubType = MEDIA_PIWG_FW_FILE_DP;
}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI DevicePathFromHandle (EFI_HANDLE Handle)
{ assert (Handle == (EFI_HANDLE)0x5678); return &ParentPath; }
EFI_DEVICE_PATH_PROTOCOL *EFIAPI AppendDevicePathNode (
  CONST EFI_DEVICE_PATH_PROTOCOL *Path, CONST EFI_DEVICE_PATH_PROTOCOL *Node)
{
  assert (Path == &ParentPath && Node->SubType == MEDIA_PIWG_FW_FILE_DP);
  return FailPath ? NULL : malloc (sizeof (*Node));
}
STATIC EFI_STATUS EFIAPI HandleProtocol (EFI_HANDLE Handle, EFI_GUID *Guid, VOID **Out)
{
  assert (Handle == gImageHandle && Guid == &gEfiLoadedImageProtocolGuid);
  ParentImage.DeviceHandle = (EFI_HANDLE)0x5678; *Out = &ParentImage;
  return ParentResult;
}
STATIC EFI_STATUS EFIAPI LoadImage (BOOLEAN Boot, EFI_HANDLE Parent,
  EFI_DEVICE_PATH_PROTOCOL *Path, VOID *Buffer, UINTN Size, EFI_HANDLE *Image)
{
  assert (!Boot && Parent == gImageHandle && Path != NULL && Buffer == NULL && Size == 0);
  ++Loads; *Image = NoImage ? NULL : (EFI_HANDLE)0x7890; return LoadResult;
}
STATIC EFI_STATUS EFIAPI StartImage (EFI_HANDLE Image, UINTN *Size, CHAR16 **Data)
{ assert (Image == (EFI_HANDLE)0x7890 && Size == NULL && Data == NULL); ++Starts; return StartResult; }
STATIC EFI_STATUS EFIAPI UnloadImage (EFI_HANDLE Image)
{ assert (Image == (EFI_HANDLE)0x7890); ++Unloads; return EFI_SUCCESS; }
STATIC VOID EFIAPI ResetSystem (EFI_RESET_TYPE Type, EFI_STATUS Status, UINTN Size, VOID *Data)
{ assert (Type == EfiResetShutdown && Size == 0 && Data == NULL); (VOID)Status; ++Resets; longjmp (ResetJump,1); }

int main (void)
{
  Services.LocateProtocol=LocateProtocol;
  Services.HandleProtocol=HandleProtocol; Services.LoadImage=LoadImage;
  Services.StartImage=StartImage; Services.UnloadImage=UnloadImage;
  Runtime.ResetSystem=ResetSystem;
  Fdt=(EFI_CR_FDT_PROTOCOL){.Revision=EFI_CR_FDT_PROTOCOL_REVISION,
    .Size=sizeof(Fdt),.GetBootInfo=GetBootInfo};
  BootInfo.Version=CR_ANDROID_BOOT_VERSION; BootInfo.Mode=CR_ANDROID_BOOT_CHARGER;
  HaveProtocol=FALSE; ChargerBootLaunch (); assert (Loads==0);
  HaveProtocol=TRUE;
  LocateResult=EFI_NOT_FOUND; ChargerBootLaunch (); assert (Loads==0);
  LocateResult=EFI_WARN_UNKNOWN_GLYPH; ChargerBootLaunch (); assert (Loads==0);
  LocateResult=EFI_SUCCESS;
  Fdt.Size--; ChargerBootLaunch (); assert (Loads==0); Fdt.Size++;
  Fdt.Revision++; ChargerBootLaunch (); assert (Loads==0); Fdt.Revision--;
  Fdt.GetBootInfo=NULL; ChargerBootLaunch (); assert (Loads==0); Fdt.GetBootInfo=GetBootInfo;
  /* Error/warning data cannot accidentally launch even when the provider wrote
     a charger-looking output. The real importer separately clears error output. */
  BootResult=EFI_COMPROMISED_DATA; ChargerBootLaunch (); assert (Loads==0);
  BootResult=EFI_WARN_UNKNOWN_GLYPH; ChargerBootLaunch (); assert (Loads==0);
  BootResult=EFI_SUCCESS;
  BootInfo.Version++; ChargerBootLaunch (); assert (Loads==0); BootInfo.Version--;
  BootInfo.Mode=CR_ANDROID_BOOT_UNKNOWN; ChargerBootLaunch (); assert (Loads==0);
  BootInfo.Mode=CR_ANDROID_BOOT_NORMAL; ChargerBootLaunch (); assert (Loads==0);
  BootInfo.Mode=CR_ANDROID_BOOT_CHARGER;
  if (!setjmp (ResetJump)) { ChargerBootLaunch (); abort (); }
  assert (Loads==1 && Starts==1 && Unloads==1 && Resets==1);
  LoadResult=EFI_SECURITY_VIOLATION;
  if (!setjmp (ResetJump)) { ChargerBootLaunch (); abort (); }
  assert (Loads==2 && Starts==1 && Unloads==1 && Resets==2);
  FailPath=TRUE;
  if (!setjmp (ResetJump)) { ChargerBootLaunch (); abort (); }
  assert (Loads==2 && Resets==3);
  ParentResult=EFI_NOT_FOUND;
  if (!setjmp (ResetJump)) { ChargerBootLaunch (); abort (); }
  assert (Loads==2 && Resets==4);
  ParentResult=EFI_WARN_UNKNOWN_GLYPH; FailPath=FALSE;
  if (!setjmp (ResetJump)) { ChargerBootLaunch (); abort (); }
  assert (Loads==2 && Starts==1 && Resets==5);
  ParentResult=EFI_SUCCESS; LoadResult=EFI_WARN_UNKNOWN_GLYPH;
  if (!setjmp (ResetJump)) { ChargerBootLaunch (); abort (); }
  assert (Loads==3 && Starts==1 && Unloads==1 && Resets==6);
  LoadResult=EFI_SUCCESS; NoImage=TRUE;
  if (!setjmp (ResetJump)) { ChargerBootLaunch (); abort (); }
  assert (Loads==4 && Starts==1 && Unloads==1 && Resets==7);
  puts ("Actual DAL FDT protocol gate and BDS charger launch/failure policy PASS");
  return 0;
}
