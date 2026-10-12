/* Pinned SM8450 Gen7 register facts; GPL-2.0-only.
 * Linux a53261b6 a6xx[_gmu].xml; Android e93f69dc gen7_reg.h.
 */
#include <Library/AdrenoStopCrLib.h>
#define R CR_ADRENO_STOP_REG_READ
#define W CR_ADRENO_STOP_REG_WRITE
#define X CR_ADRENO_STOP_REG_GX
#define GPU(O,A) {CrSm8450GpuNamespaceGpu,O,A,0}
#define GMU(O,A) {CrSm8450GpuNamespaceGmu,O,A,0}
#define RSC(O) {CrSm8450GpuNamespaceRscc,O,R,0}
STATIC CONST CR_ADRENO_STOP_REGISTER_INFO Registers[CrAdrenoStopRegisterCount]={
  GMU(0x1f8d0,R), GPU(0x38,R|W|X), GMU(0x1f993,R|W), GMU(0x23b06,R|W),
  GMU(0x1f8c3,R|W), GMU(0x1f8f1,R|W), GMU(0x23b14,W), GMU(0x23b10,R|W),
  GMU(0x1f800,R|W), GPU(0x16,R|W|X), GPU(0x17,R|X), GPU(0x3c45,R|W),
  GPU(0x3c46,R), GPU(0x43,R|W|X), GMU(0x23b0c,R), RSC(0x101),
  RSC(0x346), RSC(0x3ee), RSC(0x496), RSC(0x53e), RSC(0x5e6),
  RSC(0x68e), RSC(0x736), RSC(0x7de), RSC(0x886), RSC(0x92e),
};
CR_STATUS CrAdrenoSm8450StopDescribe(CR_ADRENO_STOP_REGISTER Reg,CR_ADRENO_STOP_REGISTER_INFO *Out) {
  if((UINT32)Reg>=CrAdrenoStopRegisterCount || Out==NULL || (UINTN)Out%4 ||
      (UINTN)Out>~(UINTN)0-sizeof(*Out)) return CR_INVALID_PARAMETER;
  *Out=Registers[Reg];return CR_SUCCESS;
}
CR_STATUS CrAdrenoSm8450StopResolve(CONST CR_SM8450_GPU_LAYOUT *Layout,
    CR_ADRENO_STOP_REGISTER Reg,CR_SM8450_GPU_REGISTER_SPAN *Out) {
  if((UINT32)Reg>=CrAdrenoStopRegisterCount) return CR_INVALID_PARAMETER;
  return CrSm8450GpuResourcesResolve(Layout,(CR_SM8450_GPU_NAMESPACE)Registers[Reg].Namespace,
      Registers[Reg].WordOffset,4,Out);
}
