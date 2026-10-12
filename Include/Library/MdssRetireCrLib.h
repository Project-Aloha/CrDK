/* Shared inherited display retirement. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/MdssCrLib.h>
typedef enum {
  CrMdssRetireDpu,CrMdssRetireDsi0,CrMdssRetireDsi1,
  CrMdssRetirePhy0,CrMdssRetirePhy1,CrMdssRetireVbif,CrMdssRetireWindows
} CR_MDSS_RETIRE_WINDOW;
typedef struct {
  VOID *Context;
  UINT32 Soc;
  /* Every operation requires exclusive ownership of the complete nonsecure
     MDSS pipeline, both DSI links, retained bus/domain/rails and mappings.
     The adapter seals and drains all former CPU/IRQ writers; this is distinct
     from a physical DMA fence. No library timer impersonates that rundown. */
  CR_STATUS (*Ready)(VOID *,CR_MDSS_RETIRE_WINDOW);
  CR_STATUS (*SealWriters)(VOID *);
  CR_STATUS (*Read32)(VOID *,CR_MDSS_RETIRE_WINDOW,UINT32,UINT32 *);
  CR_STATUS (*Write32)(VOID *,CR_MDSS_RETIRE_WINDOW,UINT32,UINT32);
  CR_STATUS (*Fence)(VOID *);
  CR_STATUS (*DelayUs)(VOID *,UINT32);
  /* Actual clock owner asserts/deasserts the whole MDSS core BCR and verifies
     each readback. The same exclusive claim covers every affected route.
     Early reset runs under the access-only lease before the first register
     access; CoreResetReleased prepares functional access after deassertion. */
  CR_STATUS (*ResetCore)(VOID *,BOOLEAN);
  CR_STATUS (*CoreResetReleased)(VOID *);
  CR_STATUS (*ExternalClocksOff)(VOID *);
  CR_STATUS (*ExternalClocksReadyOff)(VOID *);
} CR_MDSS_RETIRE_IO;
typedef struct {
  CR_MDSS_RETIRE_IO Io;
  CONST CR_MDSS_HW_PROFILE *Hardware;
  UINT32 Stage,ResumeStage,ResumeForce,OriginalForce[4];
  /* Force requests are hints to keep the clients clocked during halt.
     Completion is the fresh VBIF ACK for every XIN, not these readbacks. */
  UINT32 ForceReadback[4],ForceMissing[4];
  /* Last masked readback refusal for native diagnostics. No extra MMIO is
     issued after an access failure, and these fields confer no authority. */
  CR_MDSS_RETIRE_WINDOW FailureWindow;
  UINT32 FailureOffset,FailureMask,FailureExpected,FailureActual;
  BOOLEAN ReadbackFailed;
  BOOLEAN Initialized,Touched,WritersSealed,ForceSaved,Retired,Resumed;
} CR_MDSS_RETIRE;
/* Caller retains stable storage and exclusive serialization. Init is pure.
   A partial retirement never authorizes resource withdrawal or SMMU changes.
   Start retries only the retained stage. Check is read-only and must succeed
   again at each subsequent SMMU route-commit boundary. */
CR_STATUS CrMdssRetireInit(CR_MDSS_RETIRE *,CONST CR_MDSS_RETIRE_IO *);
CR_STATUS CrMdssRetireStart(CR_MDSS_RETIRE *);
CR_STATUS CrMdssRetireCheck(CONST CR_MDSS_RETIRE *);
/* After stopped streams have been replaced and before a new pipeline starts,
   restore the exact force-clock bits and release the retained XIN halt. This
   is not a resource release and cannot be used before the stopped-set grant.
   A partial resume invalidates Check and Start, retains all resource claims
   and retries its own journal. No new pipeline can start until it succeeds. */
CR_STATUS CrMdssRetireResume(CR_MDSS_RETIRE *);
