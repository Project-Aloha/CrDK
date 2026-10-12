/** Actual GOP entry publication and mode-clear contract, with hardware mocked. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#undef NULL
static unsigned char handoff_enable;
#define _PCD_VALUE_PcdMdssHandoffEnable handoff_enable
#define _PCD_GET_MODE_BOOL_PcdMdssHdkExtendedProfileEnable FALSE
#define _PCD_VALUE_PcdMdssHdkExtendedProfileEnable FALSE
#include "../Driver/MDSSCrDxe/MDSSCrDxe.c"
#include <Configuration/BootDevices.h>

EFI_GUID gEfiGraphicsOutputProtocolGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
EFI_GUID gEfiDevicePathProtocolGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
EFI_GUID gEfiMdpDsiProtocolGuid = { .Data1 = 1 };
EFI_GUID gEfiLt9611CrProtocolGuid = { .Data1 = 2 };
EFI_GUID gEfiMdssHandoffProtocolGuid = { .Data1 = 3 };
EFI_GUID gEfiDisplayPowerStateProtocolGuid = { .Data1 = 4 };
EFI_BOOT_SERVICES *gBS;
EFI_STATUS MdpHdkExtendedProfileStart(VOID){assert(0);return EFI_UNSUPPORTED;}
EFI_STATUS MdpHdkExtendedProfileUnload(VOID){assert(0);return EFI_UNSUPPORTED;}
BOOLEAN MdpHdkExtendedProfileRetained(VOID){assert(0);return FALSE;}
_Alignas(4096) static unsigned char fb[1920 * 1080 * 4];
static EFI_GRAPHICS_OUTPUT_PROTOCOL *published;
static EFI_MDP_DSI_PROTOCOL *prepared_dsi;
static VOID *published_path;
static unsigned installs, flushes, releases, initializes;
static UINT32 video_config, expected_lp;
static BOOLEAN fail_install, fail_uninstall, fail_init, fail_quiesce;
static UINT32 host_ctrl;
static UINT32 frame_count;
static BOOLEAN fail_progress, fail_status, fail_flush, fail_stop;
static unsigned progress_reads;
static UINT32 dsi_regs[0x200/4], intf_regs[0x300/4], ctl_regs[0x200/4], sspp_regs[0x200/4];
static unsigned ticks, host_at, init_at, video_at, publish_at, stop_at, term_at, release_at;
static unsigned init_calls, term_calls, delay120, delay20, quiesces, uninstalls;
static EFI_TPL tpl;
static const char *xml_path;
static EFI_STATUS register_status, retire_status, freeze_status, resource_snapshot_status;
static EFI_MDSS_HANDOFF_FREEZE registered_freeze;
static EFI_MDSS_HANDOFF_SNAPSHOT registered_snapshot;
static unsigned freeze_calls;
static unsigned mmio_accesses;
static EFI_STATUS EFIAPI participant(EFI_MDSS_HANDOFF_PROTOCOL *p,UINT32 o,VOID *c,EFI_MDSS_HANDOFF_FREEZE f,EFI_MDSS_HANDOFF_SNAPSHOT s) {
 assert(o==CR_MDSS_NATIVE_DSI && c==&mDisplay);
 if(f){assert(!initializes || !mDisplay.ResourcesReady);if(register_status!=EFI_SUCCESS)return register_status;}
 else if(retire_status!=EFI_SUCCESS)return retire_status;
 registered_freeze=f;registered_snapshot=s;return EFI_SUCCESS;
}
static EFI_MDSS_HANDOFF_PROTOCOL handoff={EFI_MDSS_HANDOFF_PROTOCOL_REVISION,participant};
EFI_STATUS MdpDsiCommandFreeze(VOID){freeze_calls++;return freeze_status;}
EFI_STATUS MdpSnapshotResources(CR_MDSS_HANDOFF_PARTICIPANT *p,VOID **map){assert(p->Owner==CR_MDSS_NATIVE_DSI);*map=(VOID*)1;return resource_snapshot_status;}
static void check_mmio_address(UINTN address) {
 mmio_accesses++;
 /* Independent physical windows from sm8450.dtsi plus the DPU catalog.
    Do not calculate these from the production base: that hid a 0x1000 error. */
 assert((address >= 0x0AE01000 && address < 0x0AE01494) ||
        (address >= 0x0AE16000 && address < 0x0AE16204) ||
        (address >= 0x0AE25000 && address < 0x0AE2532C) ||
        (address >= 0x0AE36000 && address < 0x0AE36300) ||
        (address >= 0x0AE45000 && address < 0x0AE45400) ||
        (address >= 0x0AE94000 && address < 0x0AE94400));
 assert((address & 3) == 0);
}
VOID *EFIAPI ZeroMem (VOID *p, UINTN n) { return memset(p, 0, n); }
VOID *EFIAPI CopyMem (VOID *p, CONST VOID *q, UINTN n) { return memcpy(p,q,n); }
INTN EFIAPI CompareMem (CONST VOID *p, CONST VOID *q, UINTN n) { return memcmp(p,q,n); }
UINTN EFIAPI AsciiStrLen (CONST CHAR8 *p) { return strlen(p); }
VOID *EFIAPI AllocatePool (UINTN n) { return malloc(n); }
VOID *EFIAPI AllocateZeroPool (UINTN n) { return calloc(1,n); }
VOID *EFIAPI AllocateCopyPool (UINTN n, CONST VOID *p) { VOID *q=malloc(n); if(q) memcpy(q,p,n); return q; }
VOID EFIAPI FreePool (VOID *p) { free(p); }
VOID EFIAPI DebugPrint (UINTN l, CONST CHAR8 *f, ...) { }
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN l) { return FALSE; }
BOOLEAN EFIAPI DebugCodeEnabled (VOID) { return FALSE; }
VOID EFIAPI MemoryFence (VOID) { }
UINTN EFIAPI MicroSecondDelay (UINTN n) { if(n==120000) delay120++; if(n==20000) delay20++; return n; }
UINT32 EFIAPI MmioRead32 (UINTN a) {
 check_mmio_address(a);
 if(a==MDP_BASE+DPU_INTF1+0x26c) return fail_stop ? 1 : (fail_status ? 0 : intf_regs[0]);
 if(a==MDP_BASE+DPU_INTF1+0xac) { progress_reads++; return fail_progress ? frame_count : frame_count++; }
 if(a==MDP_BASE+DPU_CTL0+CTL_FLUSH) return fail_flush ? 1 : 0;
 if(a==DSI0_BASE+4+DSI_CTRL) return host_ctrl;
 return a == DSI0_BASE + 4 + DSI_VERSION ? 0x10000000 : 0;
}
UINT32 EFIAPI MmioWrite32 (UINTN a, UINT32 v) {
 check_mmio_address(a);
 if(a==MDP_BASE+DPU_CTL0+CTL_FLUSH && v) {
   assert(intf_regs[INTF_PROG_FETCH_START/4]==2472801U);
   assert(intf_regs[INTF_CONFIG/4] & BIT31);
 }
 if(a>=DSI0_BASE+4 && a<DSI0_BASE+4+sizeof(dsi_regs)) dsi_regs[(a-DSI0_BASE-4)/4]=v;
 if(a>=MDP_BASE+DPU_INTF1 && a<MDP_BASE+DPU_INTF1+sizeof(intf_regs)) intf_regs[(a-MDP_BASE-DPU_INTF1)/4]=v;
 if(a>=MDP_BASE+DPU_CTL0 && a<MDP_BASE+DPU_CTL0+sizeof(ctl_regs)) ctl_regs[(a-MDP_BASE-DPU_CTL0)/4]=v;
 if(a>=MDP_BASE+DPU_SSPP_DMA0 && a<MDP_BASE+DPU_SSPP_DMA0+sizeof(sspp_regs)) sspp_regs[(a-MDP_BASE-DPU_SSPP_DMA0)/4]=v;
 if(a==DSI0_BASE+4+DSI_VID_CFG0) video_config=v;
 if(a==DSI0_BASE+4+DSI_CTRL) {
   host_ctrl=v;
   if(v & DSI_CTRL_VID_MODE_EN) video_at=++ticks;
   else if(v & DSI_CTRL_ENABLE) host_at=++ticks;
 }
 if(a==0x0AE36000 && v==0) stop_at=++ticks;
 return v;
}
UINT32 EFIAPI MmioOr32 (UINTN a, UINT32 v) { return v; }
VOID *EFIAPI WriteBackDataCacheRange (VOID *p, UINTN n) { assert(p==fb && n==sizeof(fb)); flushes++; return p; }
EFI_STATUS EFIAPI GetSectionFromAnyFv (CONST EFI_GUID *g, EFI_SECTION_TYPE t, UINTN instance, VOID **buffer, UINTN *size) {
 FILE *f=fopen(xml_path,"rb"); assert(f); fseek(f,0,SEEK_END); *size=ftell(f); rewind(f); *buffer=malloc(*size); assert(fread(*buffer,1,*size,f)==*size); fclose(f); return EFI_SUCCESS;
}
VOID *EFIAPI SetMem (VOID *P, UINTN N, UINT8 V) { return memset(P,V,N); }
EFI_STATUS EFIAPI LocateMemoryMapAreaByName (CHAR8 *name, ARM_MEMORY_REGION_DESCRIPTOR_EX *r) { assert(!strcmp(name,"Display Reserved")); r->Address=(UINTN)fb; r->Length=sizeof(fb); return EFI_SUCCESS; }
EFI_STATUS MdpInitializeResources (CONST EFI_MDP_DSI_TIMING *t) { initializes++; return EFI_SUCCESS; }
EFI_STATUS MdpGetHostClockTiming (UINT32 *v) { *v=0x123; return EFI_SUCCESS; }
EFI_STATUS MdpMapFrameBuffer (EFI_PHYSICAL_ADDRESS p, UINTN n, EFI_PHYSICAL_ADDRESS *d) { assert(p==(UINTN)fb && n==sizeof(fb)); *d=0x40000000; return EFI_SUCCESS; }
EFI_STATUS MdpReleaseResources (VOID) {
 assert(!mDisplay.ScanoutActive && !mDisplay.HostConfigured);
 releases++; release_at=++ticks; return EFI_SUCCESS;
}
EFI_STATUS MdpDsiCommandTransfer (UINT32 Controller, UINT8 VirtualChannel,
 UINT8 DataType, CONST UINT8 *Payload, UINTN Length, BOOLEAN LowPower) {
 assert(Controller==0 && VirtualChannel==0 && LowPower && Length>0);
 assert(mDisplay.HostConfigured && !mDisplay.ScanoutActive);
 assert(!(host_ctrl & DSI_CTRL_VID_MODE_EN));
 if(Payload[0]==0x11 || Payload[0]==0x51) {
   assert(host_ctrl & DSI_CTRL_ENABLE);
   init_calls++; init_at=++ticks;
   if(Payload[0]==0x11) assert(DataType==5 && Length==1);
   else assert(DataType==0x15 && Length==2 && Payload[1]==0xaa);
   return fail_init ? EFI_DEVICE_ERROR : EFI_SUCCESS;
 }
 assert(DataType==5 && Length==1 && (Payload[0]==0x28 || Payload[0]==0x10));
 term_calls++; term_at=++ticks; return EFI_SUCCESS;
}
EFI_STATUS MdpDsiCommandQuiesce (UINT32 Controller) {
 assert(Controller==0); quiesces++;
 return fail_quiesce ? EFI_NOT_READY : EFI_SUCCESS;
}
static EFI_STATUS EFIAPI bridge_config (EFI_LT9611_CR_PROTOCOL *p, CONST EFI_MDP_DSI_TIMING *t) { return EFI_SUCCESS; }
static EFI_LT9611_CR_PROTOCOL bridge={ .ConfigureVideo=bridge_config };
static EFI_STATUS EFIAPI locate (EFI_GUID *g, VOID *registration, VOID **p) { if(g==&gEfiMdssHandoffProtocolGuid){*p=&handoff;return EFI_SUCCESS;} assert(!memcmp(g,&gEfiLt9611CrProtocolGuid,sizeof(*g))); *p=&bridge; return EFI_SUCCESS; }
static EFI_TPL EFIAPI raise (EFI_TPL t) { EFI_TPL old=tpl; assert(t>=tpl); tpl=t; return old; }
static VOID EFIAPI restore (EFI_TPL t) { tpl=t; }
static EFI_STATUS EFIAPI install (EFI_HANDLE *h, ...) {
 va_list args; EFI_GUID *g; VOID *p; unsigned count=0; BOOLEAN has_gop=FALSE, has_path=FALSE, has_power=FALSE; EFI_MDP_DSI_PROTOCOL *dsi=NULL;
 va_start(args,h); while((g=va_arg(args,EFI_GUID *))!=NULL) { p=va_arg(args,VOID *); count++;
 if(!memcmp(g,&gEfiGraphicsOutputProtocolGuid,sizeof(*g))) { published=p; has_gop=TRUE; }
 else if(!memcmp(g,&gEfiDevicePathProtocolGuid,sizeof(*g))) { published_path=p; has_path=TRUE; }
 else if(!memcmp(g,&gEfiMdpDsiProtocolGuid,sizeof(*g))) dsi=p;
 else if(!memcmp(g,&gEfiDisplayPowerStateProtocolGuid,sizeof(*g))) {assert(p==&mDisplay.Power);has_power=TRUE;}
 else assert(0);
 } va_end(args);
 if (dsi != NULL) {
   /* Prepare stage publishes only the DSI handoff protocol. */
   assert(count == 1 && !has_gop && !has_path);
   prepared_dsi = dsi;
   *h = (VOID *)1;
   installs++;
   return EFI_SUCCESS;
 }
 /* BridgeReady adds the device path and GOP after scanout is live. */
 assert(count==3 && has_gop && has_path && has_power && published && published_path);
 assert(progress_reads>=2 && !fail_progress && !fail_status && !fail_flush);
 assert(!memcmp(published_path,&DisplayDevicePath,sizeof(DisplayDevicePath)));
 assert(((EFI_DISPLAY_DEVICE_PATH *)published_path)->EndDevicePath.Type==END_DEVICE_PATH_TYPE);
 installs++;
 if(fail_install) { published=NULL; published_path=NULL; return EFI_DEVICE_ERROR; }
 publish_at=++ticks; *h=(VOID *)1; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI uninstall (EFI_HANDLE h, ...) {
 EFI_GUID *g; VOID *p; BOOLEAN has_gop=FALSE, has_dsi=FALSE;
 va_list args; va_start(args,h);
 while ((g=va_arg(args,EFI_GUID *)) != NULL) {
   p=va_arg(args,VOID *); (VOID)p;
   if (!memcmp(g,&gEfiGraphicsOutputProtocolGuid,sizeof(*g))) has_gop=TRUE;
   else if (!memcmp(g,&gEfiMdpDsiProtocolGuid,sizeof(*g))) has_dsi=TRUE;
   else assert(!memcmp(g,&gEfiDevicePathProtocolGuid,sizeof(*g)) || !memcmp(g,&gEfiDisplayPowerStateProtocolGuid,sizeof(*g)));
 }
 va_end(args);
 assert(h==(VOID *)1 && (has_dsi || has_gop));
 if (has_gop) assert(mDisplay.ScanoutActive);
 uninstalls++;
 if(fail_uninstall) return EFI_ACCESS_DENIED;
 published=NULL; published_path=NULL; if (has_dsi) prepared_dsi=NULL; return EFI_SUCCESS;
}
static EFI_STATUS start_display(void) {
 EFI_STATUS status = MDSSCrDxeEntryPoint(NULL,NULL);
 if (EFI_ERROR(status)) return status;
 assert(prepared_dsi != NULL && !mDisplay.GopPublished);
 return prepared_dsi->BridgeReady(prepared_dsi);
}
static void clean (void) {
 assert(!mDisplay.Gop.Mode && !mDisplay.BltConfig && !mDisplay.Handle);
 assert(!mDisplay.Panel.InitSequence.Commands && !mDisplay.Panel.TermSequence.Commands);
  assert(!mDisplay.HostConfigured && !mDisplay.ResourcesReady && !mDisplay.ScanoutActive);
  assert(prepared_dsi == NULL);
}
/* Linux dsi_timing_setup / dpu_hw_intf_setup_timing_engine, active HDK
   1920x1080: total2200x1125, sync44x5, back porch148x36, front88x4. */
static void check_active_scanout(void) {
 assert(dsi_regs[DSI_ACTIVE_H/4]==((2112U<<16)|192U));
 assert(dsi_regs[DSI_ACTIVE_V/4]==((1121U<<16)|41U));
 assert(dsi_regs[DSI_TOTAL/4]==((1124U<<16)|2199U));
 assert(dsi_regs[DSI_ACTIVE_HSYNC/4]==44U<<16);
 assert(dsi_regs[DSI_ACTIVE_VSYNC_HPOS/4]==0);
 assert(dsi_regs[DSI_ACTIVE_VSYNC_VPOS/4]==5U<<16);
 assert((video_config & (BIT12|BIT15))==expected_lp);
 assert(intf_regs[INTF_HSYNC_CTL/4]==((2200U<<16)|44U));
 assert(intf_regs[INTF_VSYNC_PERIOD_F0/4]==2475000U);
 assert(intf_regs[INTF_VSYNC_PULSE_WIDTH_F0/4]==11000U);
 assert(intf_regs[INTF_DISPLAY_V_START_F0/4]==90200U);
 assert(intf_regs[INTF_DISPLAY_V_END_F0/4]==2466199U);
 assert(intf_regs[INTF_DISPLAY_HCTL/4]==((2111U<<16)|192U));
 assert(intf_regs[INTF_DISPLAY_DATA_HCTL/4]==((2111U<<16)|192U));
 assert(intf_regs[INTF_BORDER_COLOR/4]==mDisplay.Panel.BorderColor);
 /* Android Waipio always prefetches in video mode, with a one-line
    delay: HDK's two selected VFP lines start at the last line + 1 pixel. */
 assert(intf_regs[0x170/4]==2472801U);
 assert(intf_regs[INTF_CONFIG/4]==BIT31 && intf_regs[INTF_CONFIG2/4]==BIT4);
 assert(intf_regs[INTF_FRAME_LINE_COUNT_EN/4]==3);
 assert(intf_regs[INTF_MUX/4]==0 && intf_regs[INTF_PANEL_FORMAT/4]==0x213f);
 /* DPU 8.1 uses dpu_hw_ctl_update_pending_flush_intf_v1: the per-INTF
    mask selects INTF_1 (DSI0), while the main CTL mask uses INTF_IDX. */
 assert(ctl_regs[CTL_INTF_ACTIVE/4]==BIT1 && ctl_regs[CTL_INTF_FLUSH/4]==BIT1);
 assert(ctl_regs[CTL_FLUSH/4]==(BIT11|BIT6|BIT31|BIT17));
 assert(ctl_regs[CTL_FETCH_PIPE_ACTIVE/4]==BIT0 && ctl_regs[CTL_PIPE_ACTIVE/4]==BIT0);
 assert(ctl_regs[CTL_LAYER_ACTIVE/4]==BIT0 && ctl_regs[CTL_TOP/4]==0xf0000000);
 assert(sspp_regs[SSPP_SRC0_ADDR/4]==0x40000000 && sspp_regs[SSPP_SRC1_ADDR/4]==0);
 assert(sspp_regs[SSPP_SRC_UNPACK_PATTERN/4]==0x03020001);
 assert(sspp_regs[SSPP_SRC_YSTRIDE0/4]==7680);
}
int main(int argc,char **argv) {
 EFI_BOOT_SERVICES services={ .LocateProtocol=locate, .InstallMultipleProtocolInterfaces=install,
   .UninstallMultipleProtocolInterfaces=uninstall, .RaiseTPL=raise, .RestoreTPL=restore };
 EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info; UINTN size;
 unsigned before, old_terms, old_releases, old_installs, old_quiesces;
 assert(argc==3 || argc==4); xml_path=argv[1]; expected_lp=(UINT32)strtoul(argv[2],NULL,0); gBS=&services;
 if(argc==4 && !strncmp(argv[3],"handoff",7)) {
   handoff_enable=TRUE;
   if(!strcmp(argv[3],"handoff-register")) {
     register_status=EFI_WARN_WRITE_FAILURE;
     assert(MDSSCrDxeEntryPoint(NULL,NULL)==EFI_NOT_READY);
     assert(!registered_freeze && !mHandoffRegistered && !initializes && !mmio_accesses); clean();
     puts("MDSS handoff: failed registration prevents hardware mutation PASS"); return 0;
   }
   assert(start_display()==EFI_SUCCESS && registered_freeze && registered_snapshot && mHandoffRegistered);
   if(!strcmp(argv[3],"handoff-retire")) {
     retire_status=EFI_DEVICE_ERROR;
     assert(MDSSCrDxeUnload(NULL)==EFI_DEVICE_ERROR && mHandoffRegistered && registered_freeze);
     assert(!mDisplay.ResourcesReady && !published);
     retire_status=EFI_SUCCESS;
     assert(MDSSCrDxeUnload(NULL)==EFI_SUCCESS && !mHandoffRegistered && !registered_freeze);clean();
     puts("MDSS handoff: failed callback retirement retains image until retry PASS"); return 0;
   }
   CR_MDSS_HANDOFF_PARTICIPANT snap,prior; VOID *map=(VOID*)77;
   BOOLEAN busy=!strcmp(argv[3],"handoff-busy");
   if(!strcmp(argv[3],"handoff-fence"))freeze_status=EFI_DEVICE_ERROR;
   if(busy)mDisplay.Busy=TRUE;
   unsigned before_io=mmio_accesses,before_release=releases,before_flush=flushes;
   tpl=TPL_NOTIFY;
   assert(registered_freeze(&mDisplay)==(busy?EFI_NOT_READY:EFI_SUCCESS));
   memset(&snap,0xa5,sizeof(snap));prior=snap;
   if(busy){
     mDisplay.Busy=FALSE;
     assert(registered_freeze(&mDisplay)==EFI_NOT_READY);
     assert(registered_snapshot(&mDisplay,&snap,&map)==EFI_NOT_READY);
     assert(!memcmp(&snap,&prior,sizeof(snap)) && map==(VOID*)77 && !freeze_calls);
   }else{
     assert(registered_snapshot(&mDisplay,&snap,&map)==EFI_SUCCESS && map==(VOID*)1);
     assert(snap.Route.State==(freeze_status?CR_MDSS_NATIVE_UNCERTAIN:CR_MDSS_NATIVE_LIVE));
     assert(snap.Route.Flags==(freeze_status?3U:1U));
     assert(snap.Route.FrameBuffer==(UINTN)fb && snap.Route.FrameBufferBytes==sizeof(fb));
     assert(snap.Route.DeviceAddress==0x40000000 && !snap.Route.MappingId && !snap.Route.MappingOwner);
     assert(snap.Route.Mode.PixelHz==148500000 && snap.Route.Mode.Stride==7680);
     assert(snap.Route.InterfaceBase==0xae36000 && snap.Route.PipeBase==0xae25000);
     assert(snap.Route.LinkBase==0xae94000 && snap.Route.PhyBase==0xae94400 && snap.Route.PllBase==0xae94900);
     CR_MDSS_NATIVE_HANDOFF record={0};
     assert(CrMdssHandoffInit(&record,1,2,0x90000000,8192,31,31)==CR_SUCCESS);
     assert(CrMdssHandoffAppend(&record,&snap)==CR_SUCCESS);
     prior=snap;
     assert(registered_freeze(&mDisplay)==EFI_SUCCESS && freeze_calls==1);
     assert(registered_snapshot(&mDisplay,&snap,&map)==EFI_SUCCESS && !memcmp(&snap,&prior,sizeof(snap)));
   }
   assert(GopQueryMode(&mDisplay.Gop,0,&size,&info)==EFI_ACCESS_DENIED);
   assert(GopSetMode(&mDisplay.Gop,0)==EFI_ACCESS_DENIED);
   assert(GopBlt(&mDisplay.Gop,NULL,EfiBltVideoFill,0,0,0,0,1,1,0)==EFI_ACCESS_DENIED);
   EFI_PHYSICAL_ADDRESS address;UINT32 w,h,stride;
   assert(GetFramebuffer(&mDisplay.Dsi,&address,&w,&h,&stride)==EFI_ACCESS_DENIED);
   assert(ConfigureVideo(&mDisplay.Dsi,&mDisplay.Panel.Timing,(UINTN)fb,7680)==EFI_ACCESS_DENIED);
   assert(MdpBridgeReady(&mDisplay.Dsi)==EFI_ACCESS_DENIED);
   assert(MDSSCrDxeUnload(NULL)==EFI_ACCESS_DENIED);
   assert(mmio_accesses==before_io && releases==before_release && flushes==before_flush);
   /* Test-process teardown only; production admission is permanent. */
   tpl=0;mDisplay.Frozen=FALSE;
   assert(MDSSCrDxeUnload(NULL)==EFI_SUCCESS);clean();
   puts("MDSS handoff: TPL16 freeze, immutable snapshot, busy refusal and late admission PASS");return 0;
 }
 if(argc==4 && !strcmp(argv[3],"baseline")) {
   assert(start_display()==EFI_SUCCESS);
   assert(published && mDisplay.ScanoutActive && !mDisplay.Panel.HasCommands);
   check_active_scanout();
   assert(init_calls==0 && term_calls==0 && initializes==1);
   assert(MDSSCrDxeUnload(NULL)==EFI_SUCCESS); clean();
   assert(releases==1 && init_calls==0 && term_calls==0);
   puts("MDSS GOP: original empty-sequence LT XML entry and unload passed");
   return 0;
 }
 if(argc==4) {
   assert(MDSSCrDxeEntryPoint(NULL,NULL)==EFI_UNSUPPORTED);
   clean(); assert(initializes==0 && installs==0 && init_calls==0 && term_calls==0 && flushes==0);
   puts("MDSS GOP: oversized sequence rejected before resources or partial execution");
   return 0;
 }
 frame_count=0xffffffff; /* Startup must accept counter wrap. */
 memset(fb,0xa5,sizeof(fb));
 assert(start_display()==EFI_SUCCESS && installs==2);
 check_active_scanout();
 assert(intf_regs[INTF_BORDER_COLOR/4]==0x123456);
 assert(host_at<init_at && init_at<video_at && video_at<publish_at);
 assert(init_calls==2 && delay120==1);
 /* LoadPanelXml has freed its source allocation before transport reads bytes. */
 assert(mDisplay.Panel.InitSequence.Commands[2].Payload[1]==0xaa);
 assert((video_config & (BIT12 | BIT15))==expected_lp);
 assert(published->QueryMode(published,0,&size,&info)==EFI_SUCCESS);
 assert(size==sizeof(*info) && info->HorizontalResolution==1920 && info->VerticalResolution==1080); free(info);
 memset(fb,0xa5,sizeof(fb)); before=flushes;
 assert(published->SetMode(published,1)==EFI_UNSUPPORTED && fb[0]==0xa5);
 assert(published->SetMode(published,0)==EFI_SUCCESS && flushes==before+1 && tpl==0);
 for(size=0;size<sizeof(fb);size++) assert(fb[size]==0);
 /* Charger display-power stops actual video while preserving GOP/map/leases,
    and wakes without replaying already-owned panel preparation. */
 EFI_DISPLAY_POWER_STATE PowerState=EfiDisplayPowerStateUnknown;
 unsigned PowerInits=init_calls,PowerReleases=releases;
 EFI_PHYSICAL_ADDRESS PowerFb=mDisplay.Gop.Mode->FrameBufferBase;
 assert(DisplayPowerGet(&mDisplay.Power,&PowerState)==EFI_SUCCESS && PowerState==EfiDisplayPowerStateOn);
 assert(DisplayPowerSet(&mDisplay.Power,EfiDisplayPowerStateOff)==EFI_SUCCESS);
 assert(!mDisplay.ScanoutActive && mDisplay.ResourcesReady && mDisplay.DeviceFrameBuffer==0x40000000);
 assert(DisplayPowerGet(&mDisplay.Power,&PowerState)==EFI_SUCCESS && PowerState==EfiDisplayPowerStateOff);
 assert(published && mDisplay.Gop.Mode->FrameBufferBase==PowerFb && releases==PowerReleases);
 assert(DisplayPowerSet(&mDisplay.Power,EfiDisplayPowerStateOn)==EFI_SUCCESS);
 assert(init_calls==PowerInits && mDisplay.ScanoutActive);
 fail_stop=TRUE;assert(DisplayPowerSet(&mDisplay.Power,EfiDisplayPowerStateOff)==EFI_TIMEOUT);
 PowerState=EfiDisplayPowerStateUnknown;
 assert(DisplayPowerGet(&mDisplay.Power,&PowerState)==EFI_NOT_READY && PowerState==EfiDisplayPowerStateUnknown);
 assert(mDisplay.PowerPending && releases==PowerReleases && published);
 CR_MDSS_HANDOFF_PARTICIPANT PendingSnapshot;VOID *PendingMap=NULL;
 assert(FreezeDisplay(&mDisplay)==EFI_SUCCESS);
 assert(SnapshotDisplay(&mDisplay,&PendingSnapshot,&PendingMap)==EFI_SUCCESS);
 assert(PendingSnapshot.Route.State==CR_MDSS_NATIVE_UNCERTAIN &&
        (PendingSnapshot.Route.Flags&CR_MDSS_NATIVE_RETAINED_ERROR));
 mDisplay.Frozen=FALSE;mDisplay.FreezeComplete=FALSE;
 tpl=TPL_NOTIFY;assert(DisplayPowerSet(&mDisplay.Power,EfiDisplayPowerStateOn)==EFI_UNSUPPORTED);tpl=0;
 fail_stop=FALSE;assert(DisplayPowerSet(&mDisplay.Power,EfiDisplayPowerStateOn)==EFI_SUCCESS);
 assert(!mDisplay.PowerPending && mDisplay.ScanoutActive && init_calls==PowerInits);
 mDisplay.Frozen=TRUE;unsigned PowerIo=mmio_accesses;
 assert(DisplayPowerSet(&mDisplay.Power,EfiDisplayPowerStateOff)==EFI_ACCESS_DENIED);
 assert(DisplayPowerGet(&mDisplay.Power,&PowerState)==EFI_ACCESS_DENIED && mmio_accesses==PowerIo);
 mDisplay.Frozen=FALSE;
 /* Consumer refusal must not stop video, release mappings, or send off bytes. */
 fail_uninstall=TRUE; old_terms=term_calls; old_releases=releases; old_quiesces=quiesces;
 assert(MDSSCrDxeUnload(NULL)==EFI_ACCESS_DENIED);
 assert(mDisplay.ScanoutActive && published && mDisplay.Handle);
 assert(term_calls==old_terms && releases==old_releases && quiesces==old_quiesces);
 fail_uninstall=FALSE;
 assert(MDSSCrDxeUnload(NULL)==EFI_SUCCESS); clean();
 assert(term_calls==old_terms+2 && stop_at<term_at && term_at<release_at);
 assert(MDSSCrDxeUnload(NULL)==EFI_SUCCESS); clean();
 /* No GOP until status, frame progress AND consumed flush are observed.
    Failed startup must still stop scanout before term/resource release. */
 for(unsigned failure=0; failure<3; failure++) {
   fail_progress=failure==0; fail_status=failure==1; fail_flush=failure==2;
   old_installs=installs; old_releases=releases; before=stop_at;
   assert(start_display()==EFI_TIMEOUT);
   clean(); assert(installs==old_installs+1 && releases==old_releases+1 && !published);
   assert(stop_at>before && stop_at<term_at && term_at<release_at);
 }
 fail_progress=fail_status=fail_flush=FALSE;
 /* A startup timeout followed by an unacknowledged stop retains DMA. */
 fail_progress=fail_stop=TRUE; old_releases=releases; old_installs=installs;
 assert(start_display()==EFI_SUCCESS);
 assert(!published && mDisplay.ScanoutActive && mDisplay.ResourcesReady);
 assert(installs==old_installs+1 && releases==old_releases);
 fail_progress=fail_stop=FALSE;
 assert(MDSSCrDxeUnload(NULL)==EFI_SUCCESS); clean();
 assert(releases==old_releases+1);
 fail_install=TRUE; old_releases=releases; old_terms=term_calls; old_installs=installs;
 assert(start_display()==EFI_DEVICE_ERROR);
 clean(); assert(releases==old_releases+1 && term_calls==old_terms+2 && installs==old_installs+2 && !published);
 fail_install=FALSE;
 /* Init error with a successful fence cleans up and fails entry. */
 fail_init=TRUE; old_releases=releases; old_terms=term_calls; old_installs=installs;
 assert(start_display()==EFI_DEVICE_ERROR);
 clean(); assert(releases==old_releases+1 && term_calls==old_terms+2 && installs==old_installs+1);
 /* An unfenced transfer keeps driver ownership resident with no GOP. */
 fail_quiesce=TRUE; old_releases=releases; old_terms=term_calls; old_installs=installs;
 assert(start_display()==EFI_SUCCESS);
 assert(!published && mDisplay.Handle && installs==old_installs+1);
 assert(mDisplay.ResourcesReady && mDisplay.HostConfigured && !mDisplay.ScanoutActive);
 assert(mDisplay.Panel.InitSequence.Commands && mDisplay.Gop.Mode && mDisplay.BltConfig);
 assert(releases==old_releases && term_calls==old_terms);
 {
   VOID *owned=mDisplay.Panel.InitSequence.Commands;
   assert(MDSSCrDxeEntryPoint(NULL,NULL)==EFI_ALREADY_STARTED);
   assert(mDisplay.Panel.InitSequence.Commands==owned && mDisplay.ResourcesReady && mDisplay.HostConfigured);
 }
 assert(MDSSCrDxeUnload(NULL)==EFI_NOT_READY);
 assert(releases==old_releases && term_calls==old_terms);
 fail_quiesce=FALSE; fail_init=FALSE;
 assert(MDSSCrDxeUnload(NULL)==EFI_SUCCESS); clean();
 assert(releases==old_releases+1 && term_calls==old_terms+2);
 puts("MDSS GOP: ordered init/video/term, actual unload, failure fencing, resident retry and owned XML passed");
 return 0;
}
RETURN_STATUS EFIAPI FrameBufferBltConfigure (VOID *p, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *i, FRAME_BUFFER_CONFIGURE *c, UINTN *n) { *n=8; return c ? RETURN_SUCCESS : RETURN_BUFFER_TOO_SMALL; }
RETURN_STATUS EFIAPI FrameBufferBlt (FRAME_BUFFER_CONFIGURE *c, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *p, EFI_GRAPHICS_OUTPUT_BLT_OPERATION o, UINTN sx,UINTN sy,UINTN dx,UINTN dy,UINTN w,UINTN h,UINTN delta) { return RETURN_SUCCESS; }
