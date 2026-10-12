/* A730.1 upstream profile, Linux a53261b6 hw_init and helpers.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "Internal.h"
#include "A730Tables.h"
#include "A730ProfileRegisters.h"
#define TRY(Call) do{CR_STATUS S_=(Call);if(S_!=CR_SUCCESS)return S_;}while(0)
#define W(R,V) TRY(CpWrite(C,CrAdrenoCpGpu,(R),(V)))
#define G(R,V) TRY(CpWrite(C,CrAdrenoCpGmu,(R),(V)))
CR_STATUS CpConfigureA730(CR_ADRENO_CP_CONTEXT *C){
 UINT32 V=0;
 W(REG_A6XX_GBIF_HALT,0);TRY(CpRead(C,CrAdrenoCpGpu,REG_A6XX_GBIF_HALT,&V));if(V)return CR_DEVICE_ERROR;
 W(REG_A6XX_RBBM_GBIF_HALT,0);TRY(CpRead(C,CrAdrenoCpGpu,REG_A6XX_RBBM_GBIF_HALT,&V));if(V)return CR_DEVICE_ERROR;
 W(REG_A6XX_RBBM_SECVID_TSB_CNTL,0);
 TRY(CpWrite64(C,CrAdrenoCpGpu,REG_A6XX_RBBM_SECVID_TSB_TRUSTED_BASE,0));W(REG_A6XX_RBBM_SECVID_TSB_TRUSTED_SIZE,0);
 /* Original HWCG helper includes these GMU writes before its inherited-state
    shortcut. The clock provider's dedicated ownership must cover them. */
 G(REG_A6XX_GPU_GMU_AO_GMU_CGC_MODE_CNTL,0x20000);
 G(REG_A6XX_GPU_GMU_AO_GMU_CGC_DELAY_CNTL,0x10111);
 G(REG_A6XX_GPU_GMU_AO_GMU_CGC_HYST_CNTL,0x5555);
 TRY(CpRead(C,CrAdrenoCpGpu,REG_A6XX_RBBM_CLOCK_CNTL,&V));
 if(V!=0x8aa8aa82){for(UINT32 N=0;N<sizeof(Hwcg)/sizeof(Hwcg[0]);++N)W(Hwcg[N][0],Hwcg[N][1]);W(REG_A6XX_RBBM_CLOCK_CNTL,0x8aa8aa82);}
 W(REG_A6XX_RBBM_GBIF_CLIENT_QOS_CNTL,0x2120212);W(REG_A6XX_UCHE_GBIF_GX_CONFIG,0x10240e0);
 W(REG_A6XX_RBBM_PERFCTR_GPU_BUSY_MASKED,0xffffffff);
 TRY(CpWrite64(C,CrAdrenoCpGpu,REG_A6XX_UCHE_TRAP_BASE,0x1fffffffff000ULL));
 TRY(CpWrite64(C,CrAdrenoCpGpu,REG_A6XX_UCHE_WRITE_THRU_BASE,0x1fffffffff000ULL));
 /* A730 deliberately omits GMEM range and legacy ROQ/MEM_POOL programming. */
 W(REG_A6XX_UCHE_CACHE_WAYS,1U<<23);W(REG_A6XX_CP_AHB_CNTL,1);W(REG_A6XX_RBBM_PERFCTR_CNTL,1);
 G(REG_A6XX_GMU_CX_GMU_POWER_COUNTER_SELECT_1,4);W(REG_A6XX_CP_PERFCTR_CP_SEL_REG,0);
 /* SM8450 qcom UBWC4.0/hbb16: LVL2/3 enabled, AMSBC,32-byte minimum,8-channel.
    This is a fixed SoC profile, not an inferred property of a firmware name. */
 W(REG_A6XX_RB_NC_MODE_CNTL,0x816);W(REG_A6XX_TPL1_NC_MODE_CNTL,6);W(REG_A6XX_SP_NC_MODE_CNTL,0x26);
 for(UINT32 Pipe=1;Pipe<=2;++Pipe){W(REG_A7XX_CP_APERTURE_CNTL_HOST,Pipe<<12);W(REG_A7XX_GRAS_NC_MODE_CNTL,0x60);}
 W(REG_A7XX_CP_APERTURE_CNTL_HOST,0);W(REG_A6XX_UCHE_MODE_CNTL,0x600000);W(REG_A6XX_RBBM_NC_MODE_CNTL,1);
 W(REG_A6XX_RBBM_INTERFACE_HANG_INT_CNTL,0x40cfffff);W(REG_A6XX_UCHE_CLIENT_PF,0x81);
 G(REG_A6XX_GPU_GMU_AO_GPU_CX_BUSY_MASK,0xff000000);
 TRY(CpRmw(C,CrAdrenoCpGmu,REG_A6XX_GMU_CX_GMU_POWER_COUNTER_SELECT_0,0xff,0x20));
 G(REG_A6XX_GMU_CX_GMU_POWER_COUNTER_ENABLE,1);
 W(REG_A6XX_CP_PROTECT_CNTL,0xb);
 for(UINT32 N=0;N<47;++N)if(Protect[N])W(REG_A6XX_CP_PROTECT_REG+N,Protect[N]);
 W(REG_A6XX_CP_PROTECT_REG+47,Protect[47]);
 W(REG_A6XX_UCHE_CMDQ_CONFIG,0x6690e);
 W(REG_A6XX_CP_APRIV_CNTL,0x6f);W(REG_A7XX_CP_BV_APRIV_CNTL,0xf);W(REG_A7XX_CP_LPAC_APRIV_CNTL,0xf);
 TRY(CpRmw(C,CrAdrenoCpGpu,REG_A6XX_RB_CMP_DBG_ECO_CNTL,1U<<11,1U<<11));
 /* Polling probe owns no RBBM CPU IRQ. Begin required its mask to be zero;
    leave it zero rather than copying upstream's unowned IRQ-enabling write.
    Completion/event/status are polled; live selection must review this delta. */
 TRY(CpRead(C,CrAdrenoCpGpu,RBBM_INT_0_MASK,&V));return V?CR_BUSY:CR_SUCCESS;
}
CR_STATUS CpBuildPowerListA730(CR_ADRENO_CP_CONTEXT *C){
 volatile UINT32 *P=C->Backing.Region[CrAdrenoCpPowerList].Cpu;UINT32 At=4,V;
 for(UINT32 N=0;N<CR_ADRENO_CP_POWER_BYTES/4;++N)P[N]=0;
 for(UINT32 N=0;N<sizeof(PowerRegisters)/sizeof(PowerRegisters[0]);++N){
  TRY(CpRead(C,CrAdrenoCpGpu,PowerRegisters[N],&V));P[At++]=PowerRegisters[N];P[At++]=V;}
 for(UINT32 Pipe=1;Pipe<=2;++Pipe){W(REG_A7XX_CP_APERTURE_CNTL_HOST,Pipe<<12);
  TRY(CpRead(C,CrAdrenoCpGpu,REG_A7XX_GRAS_NC_MODE_CNTL,&V));P[At++]=Pipe<<12;P[At++]=REG_A7XX_GRAS_NC_MODE_CNTL;P[At++]=V;}
 W(REG_A7XX_CP_APERTURE_CNTL_HOST,0);
 /* cpu_gpu_lock: zero gpu_req/cpu_req/turn, u8 IFPC0, u8 preempt16,
    u16 dynamic2. Original A730 catalog has no IFPC register list. */
 P[3]=(16U<<8)|(2U<<16);return CR_SUCCESS;
}
