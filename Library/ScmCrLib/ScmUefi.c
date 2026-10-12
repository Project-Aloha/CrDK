/* Native transport adapter; not selected by any platform. MIT */
#include <Library/ScmCrLib.h>
#include <Library/ArmLib.h>
#include <Library/ArmSmcLib.h>
VOID CrPilArm64Smccc(CR_PIL_SMC_ARGS *A) {
  ARM_SMC_ARGS S={0};
  S.Arg0=A->X[0];S.Arg1=A->X[1];S.Arg2=A->X[2];S.Arg3=A->X[3];
  S.Arg4=A->X[4];S.Arg5=A->X[5];S.Arg6=A->X[6];S.Arg7=A->X[7];
  ArmCallSmc(&S);
  A->X[0]=S.Arg0;A->X[1]=S.Arg1;A->X[2]=S.Arg2;A->X[3]=S.Arg3;
  A->X[4]=S.Arg4;A->X[5]=S.Arg5;A->X[6]=S.Arg6;A->X[7]=S.Arg7;
}
UINT32 CrPilArm64CurrentEl(VOID) {return (UINT32)(ArmReadCurrentEL()>>2);}
