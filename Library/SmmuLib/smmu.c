/** @file
 *  SMMUv2/MMU-500 domains preserving firmware-owned streams and banks.
 *  SPDX-License-Identifier: MIT
 */
#include <Library/smmu.h>

#define SMR(n)      (0x800U + 4U * (n))
#define S2CR(n)     (0xC00U + 4U * (n))
#define VALID      0x80000000U
#define TRANS_MASK 0x00030000U
#define PAGE_DESC  0x0060000000000743ULL /* UXN, PXN, AF, ISH, unprivileged */

STATIC UINT32 Read(SmmuDevice *D, UINT64 Address) {
  return D->Io.Read32(D->Io.Cookie, Address);
}

STATIC VOID Write(SmmuDevice *D, UINT64 Address, UINT32 Value) {
  D->Io.Write32(D->Io.Cookie, Address, Value);
}

STATIC VOID Zero(UINT64 *P, UINTN Count) {
  while (Count-- != 0) {
    *P++ = 0;
  }
}

STATIC UINT64 BankBase(SmmuDevice *D, UINT16 Bank) {
  return D->ContextBase + (UINT64)Bank * D->PageSize;
}

STATIC BOOLEAN ContextBankInUse(SmmuDevice *D, UINT16 Bank) {
  UINT64 Cb = BankBase(D, Bank);

  /*
   * SCTLR.M is the architectural live bit.  A firmware hand-off can leave a
   * context disabled while its translation state is still reserved, however,
   * so also treat any non-zero table/configuration register as owned.  This
   * conservative check is what prevents a cold-init adapter from clobbering a
   * bank which another firmware component will re-enable later.
   */
  return (Read(D, Cb) & 1U) != 0 ||
         Read(D, Cb + 0x20) != 0 || Read(D, Cb + 0x28) != 0 ||
         Read(D, Cb + 0x30) != 0 || Read(D, Cb + 0x38) != 0 ||
         Read(D, D->Base + D->PageSize + Bank * 4U) != 0 ||
         Read(D, D->Base + D->PageSize + 0x800U + Bank * 4U) != 0;
}

STATIC CR_STATUS Sync(SmmuDomain *Domain) {
  SmmuDevice *D = Domain->Device;
  UINT32 Wait;

  Write(D, Domain->BankBase + 0x610, Domain->Asid); /* TLBIASID */
  Write(D, Domain->BankBase + 0x7F0, ~0U);         /* Qualcomm TLBSYNC dummy */
  for (Wait = 0; Wait < 10000; Wait++) {
    if ((Read(D, Domain->BankBase + 0x7F4) & 1U) == 0) {
      return CR_SUCCESS;
    }
    D->Io.DelayUs(D->Io.Cookie, 1);
  }
  Domain->Failed = TRUE;
  return CR_TIMEOUT;
}

CR_STATUS SmmuProbe(SmmuDevice *D, UINT64 Base, UINT64 Size,
                    CONST SmmuIoOps *Io) {
  STATIC CONST UINT8 Bits[] = {32, 36, 40, 42, 44, 48};
  UINT32 Id0, Id1, Id2, Shift, Pages, Encoding, Control;
  UINT16 I;

  if (D == NULL || Io == NULL || Io->Read32 == NULL || Io->Write32 == NULL ||
      Io->Write64 == NULL || Io->Publish == NULL || Io->DelayUs == NULL ||
      Base == 0 || (Base & 0xFFF) != 0 || Size < 0x10000 ||
      Base > ~0ULL - Size) {
    return CR_INVALID_PARAMETER;
  }
  D->Io = *Io;
  Id0 = Read(D, Base + 0x20);
  Id1 = Read(D, Base + 0x24);
  Id2 = Read(D, Base + 0x28);
  /* SMS, stage 1, AArch64 4K. Extended stream IDs are deliberately excluded. */
  Control = Read(D, Base);
  if ((Id0 & 0x48000000U) != 0x48000000U || (Id2 & 0x1000U) == 0 ||
      (Control & 8U) != 0) {
    return CR_UNSUPPORTED;
  }
  Shift = (Id1 & VALID) != 0 ? 16 : 12;
  Pages = 1U << (((Id1 >> 28) & 7U) + 1U);
  D->BankCount = (UINT16)(Id1 & 0xFF);
  D->Stage2BankCount = (UINT16)((Id1 >> 16) & 0xFF);
  /* Qualcomm firmware may not emulate groups above 127 correctly. */
  D->StreamCount = (UINT16)(Id0 & 0xFF);
  if (D->StreamCount > 128) {
    D->StreamCount = 128;
  }
  Encoding = Id2 & 0xF; /* IPA size for stage 1, also bounded by output size. */
  if (((Id2 >> 4) & 0xF) < Encoding) {
    Encoding = (Id2 >> 4) & 0xF;
  }
  if (Encoding >= sizeof(Bits) || D->BankCount <= D->Stage2BankCount ||
      D->BankCount > Pages || D->StreamCount == 0 ||
      ((UINT64)Pages << (Shift + 1)) > Size) {
    return CR_UNSUPPORTED;
  }
  D->Base = Base;
  D->PageSize = 1U << Shift;
  D->ContextBase = Base + ((UINT64)Pages << Shift);
  D->AddressBits = Bits[Encoding];
  D->AddressEncoding = (UINT8)Encoding;
  if ((Control & 1U) != 0) {
    /* Only an entirely unused controller can be cold-enabled here. */
    for (I = 0; I < D->StreamCount; I++) {
      if ((Read(D, Base + SMR(I)) & VALID) != 0) {
        return CR_BUSY;
      }
    }
    for (I = 0; I < D->BankCount; I++) {
      if (ContextBankInUse(D, I)) {
        return CR_BUSY;
      }
    }
    /* USFCFG=1: unmatched streams fault. Interrupts remain masked. */
    Control = (Control & ~1U) | 0xC12U; /* USFCFG, VMIDPNE, GCFGFRE, GFRE */
    Write(D, Base, Control);
    if (Read(D, Base) != Control) {
      return CR_DEVICE_ERROR;
    }
  }
  return CR_SUCCESS;
}

CR_STATUS SmmuAttach(SmmuDevice *D, SmmuDomain *Domain, UINT16 Sid,
                     VOID *Tables, UINT64 TableAddress) {
  BOOLEAN Used[256] = {FALSE};
  UINT16 I, Bank, Stream, Asid;
  UINT32 Smr, S2cr, Mask, Expected;
  UINT64 *T = Tables;
  UINT64 Cb;
  CR_STATUS Status;

  if (D == NULL || Domain == NULL || Tables == NULL || D->StreamCount == 0 ||
      (((UINTN)Tables | TableAddress) & 0xFFF) != 0 || Sid > 0x7FFF ||
      TableAddress >= (1ULL << D->AddressBits) ||
      SMMU_TABLE_PAGES * SMMU_PAGE_SIZE > (1ULL << D->AddressBits) - TableAddress) {
    return CR_INVALID_PARAMETER;
  }
  Stream = 0xFFFF;
  /* Do not disturb any valid stream, including a firmware bypass mapping.
   * Board handoff must release its matching entry before attaching here. */
  for (I = 0; I < D->StreamCount; I++) {
    Smr = Read(D, D->Base + SMR(I));
    S2cr = Read(D, D->Base + S2CR(I));
    if ((Smr & VALID) != 0) {
      Mask = (Smr >> 16) & 0x7FFF;
      if (((Sid ^ Smr) & ~Mask & 0xFFFF) == 0) {
        return CR_BUSY;
      }
      if ((S2cr & TRANS_MASK) == 0) {
        Used[S2cr & 0xFF] = TRUE;
      }
    } else if (Stream == 0xFFFF) {
      Stream = I;
    }
  }
  /* Keep the final context bank available for Qualcomm's bypass quirk. */
  for (Bank = D->Stage2BankCount; Bank + 1U < D->BankCount; Bank++) {
    if (!Used[Bank] && !ContextBankInUse(D, Bank)) {
      break;
    }
  }
  if (Stream == 0xFFFF || Bank + 1U >= D->BankCount) {
    return CR_OUT_OF_RESOURCES;
  }
  /* Avoid ASID aliases with live firmware contexts. */
  for (Asid = 1; Asid < 256; Asid++) {
    for (I = 0; I < D->BankCount; I++) {
      if (I != Bank && (Read(D, BankBase(D, I)) & 1U) != 0 &&
          ((Read(D, BankBase(D, I) + 0x24) >> 16) == Asid ||
           (Read(D, BankBase(D, I) + 0x2C) >> 16) == Asid)) {
        break;
      }
    }
    if (I == D->BankCount) {
      break;
    }
  }
  if (Asid == 256) {
    return CR_OUT_OF_RESOURCES;
  }
  Domain->Device = D;
  Domain->Tables = T;
  Domain->TableAddress = TableAddress;
  Domain->Bank = Bank;
  Domain->Stream = Stream;
  Domain->Asid = Asid;
  Domain->Sid = Sid;
  Domain->Active = FALSE;
  Domain->Failed = FALSE;
  Cb = Domain->BankBase = BankBase(D, Bank);
  Domain->OriginalSmr   = Read(D, D->Base + SMR(Stream));
  Domain->OriginalS2cr  = Read(D, D->Base + S2CR(Stream));
  Domain->OriginalSctlr = Read(D, Cb);
  Domain->OriginalCbar  = Read(D, D->Base + D->PageSize + Bank * 4U);
  Domain->OriginalCba2r = Read(D, D->Base + D->PageSize + 0x800U + Bank * 4U);
  Domain->OriginalTcr2  = Read(D, Cb + 0x10);
  Domain->OriginalTcr   = Read(D, Cb + 0x30);
  Domain->OriginalTtbr0 = ((UINT64)Read(D, Cb + 0x24) << 32) |
                          Read(D, Cb + 0x20);
  Domain->OriginalTtbr1 = ((UINT64)Read(D, Cb + 0x2C) << 32) |
                          Read(D, Cb + 0x28);
  Domain->OriginalMair0 = Read(D, Cb + 0x38);
  Domain->OriginalMair1 = Read(D, Cb + 0x3C);
  Zero(T, SMMU_TABLE_PAGES * SMMU_PAGE_SIZE / sizeof(UINT64));
  T[0] = (TableAddress + SMMU_PAGE_SIZE) | 3;
  for (I = 0; I < 32; I++) {
    T[512 + (SMMU_IOVA_BASE >> 21) + I] =
        (TableAddress + (UINT64)(I + 2) * SMMU_PAGE_SIZE) | 3;
  }
  D->Io.Publish(D->Io.Cookie, T, SMMU_TABLE_PAGES * SMMU_PAGE_SIZE);
  Write(D, D->Base + D->PageSize + 0x800 + Bank * 4, 1); /* CBA2R.VA64 */
  Write(D, D->Base + D->PageSize + Bank * 4, 0x1F300);   /* S1, S2 bypass */
  Write(D, Cb + 0x10, (7U << 15) | D->AddressEncoding); /* TCR2 */
  Write(D, Cb + 0x30, (1U << 23) | 0x3520); /* EPD1, ISH, WBWA, T0SZ=32 */
  D->Io.Write64(D->Io.Cookie, Cb + 0x20, TableAddress | ((UINT64)Asid << 48));
  D->Io.Write64(D->Io.Cookie, Cb + 0x28, (UINT64)Asid << 48);
  Write(D, Cb + 0x38, 0x44); /* MAIR0[0]: normal non-cacheable */
  Write(D, Cb + 0x3C, 0);
  Write(D, Cb + 0x58, Read(D, Cb + 0x58)); /* clear stale W1C faults */
  /* No stall or IRQ: translation faults terminate the DMA transaction. */
  Write(D, Cb, 0x1027); /* ASIDPNE, CFRE, AFE, TRE, M */
  if ((Read(D, Cb) & 0x1027) != 0x1027 ||
      Read(D, Cb + 0x20) != (UINT32)TableAddress ||
      Read(D, Cb + 0x24) != (UINT32)((TableAddress >> 32) | ((UINT64)Asid << 16))) {
    Domain->Failed = TRUE;
    return CR_DEVICE_ERROR;
  }
  Status = Sync(Domain);
  if (CR_ERROR(Status)) {
    return Status;
  }
  /* Translation to an empty table avoids Qualcomm's ignored FAULT writes. */
  Expected = (2U << 24) | Bank; /* force unprivileged accesses */
  Write(D, D->Base + S2CR(Stream), Expected);
  if (Read(D, D->Base + S2CR(Stream)) != Expected) {
    Domain->Failed = TRUE;
    return CR_DEVICE_ERROR;
  }
  Write(D, D->Base + SMR(Stream), VALID | Sid);
  if (Read(D, D->Base + SMR(Stream)) != (VALID | Sid)) {
    Domain->Failed = TRUE;
    return CR_DEVICE_ERROR;
  }
  Domain->Active = TRUE;
  return CR_SUCCESS;
}

CR_STATUS SmmuSetMapping(SmmuDomain *Domain, UINT64 Iova, UINT64 Physical,
                         UINTN Pages, UINT32 Access) {
  UINT64 *Leaf, Desc;
  UINTN I;
  SmmuDevice *D;

  if (Domain == NULL || !Domain->Active || Domain->Failed) {
    return CR_DEVICE_ERROR;
  }
  D = Domain->Device;
  if (Pages == 0 || Pages > SMMU_IOVA_SIZE / SMMU_PAGE_SIZE ||
      (Access & ~3U) != 0 || ((Iova | Physical) & 0xFFF) != 0 ||
      Iova < SMMU_IOVA_BASE || Iova - SMMU_IOVA_BASE >= SMMU_IOVA_SIZE ||
      Pages > (SMMU_IOVA_SIZE - (Iova - SMMU_IOVA_BASE)) / SMMU_PAGE_SIZE ||
      (Access != 0 && (Physical >= (1ULL << D->AddressBits) ||
       Pages > ((1ULL << D->AddressBits) - Physical) / SMMU_PAGE_SIZE))) {
    return CR_INVALID_PARAMETER;
  }
  Leaf = Domain->Tables + 1024 + (Iova - SMMU_IOVA_BASE) / SMMU_PAGE_SIZE;
  /* Break before make, including permission changes. */
  Zero(Leaf, Pages);
  D->Io.Publish(D->Io.Cookie, Leaf, Pages * sizeof(*Leaf));
  if (CR_ERROR(Sync(Domain))) {
    return CR_TIMEOUT;
  }
  if (Access != 0) {
    Desc = PAGE_DESC | ((Access & SMMU_ACCESS_WRITE) == 0 ? 0x80 : 0);
    for (I = 0; I < Pages; I++) {
      Leaf[I] = (Physical + I * SMMU_PAGE_SIZE) | Desc;
    }
    D->Io.Publish(D->Io.Cookie, Leaf, Pages * sizeof(*Leaf));
    return Sync(Domain);
  }
  return CR_SUCCESS;
}

CR_STATUS SmmuBlock(SmmuDomain *Domain) {
  if (Domain == NULL || !Domain->Active) {
    return CR_INVALID_PARAMETER;
  }
  Zero(Domain->Tables + 1024, (SMMU_TABLE_PAGES - 2) * 512);
  Domain->Device->Io.Publish(Domain->Device->Io.Cookie,
      Domain->Tables + 1024, (SMMU_TABLE_PAGES - 2) * SMMU_PAGE_SIZE);
  return Sync(Domain);
}

CR_STATUS SmmuDetach(SmmuDomain *Domain) {
  SmmuDevice *D;
  UINT64 Cb;
  CR_STATUS Status;

  if (Domain == NULL || !Domain->Active || Domain->Device == NULL) {
    return CR_INVALID_PARAMETER;
  }
  if (Domain->Failed) {
    return CR_DEVICE_ERROR;
  }

  D  = Domain->Device;
  Cb = Domain->BankBase;

  /* Break the stream mapping before touching the context-bank registers. */
  Write(D, D->Base + SMR(Domain->Stream), Domain->OriginalSmr & ~VALID);
  Status = Sync(Domain);
  if (CR_ERROR(Status)) {
    return Status;
  }

  Write(D, Cb, 0);
  Status = Sync(Domain);
  if (CR_ERROR(Status)) {
    return Status;
  }

  D->Io.Write64(D->Io.Cookie, Cb + 0x20, Domain->OriginalTtbr0);
  D->Io.Write64(D->Io.Cookie, Cb + 0x28, Domain->OriginalTtbr1);
  Write(D, Cb + 0x10, Domain->OriginalTcr2);
  Write(D, Cb + 0x30, Domain->OriginalTcr);
  Write(D, Cb + 0x38, Domain->OriginalMair0);
  Write(D, Cb + 0x3C, Domain->OriginalMair1);
  Write(D, D->Base + D->PageSize + 0x800U + Domain->Bank * 4U,
        Domain->OriginalCba2r);
  Write(D, D->Base + D->PageSize + Domain->Bank * 4U,
        Domain->OriginalCbar);
  Write(D, D->Base + S2CR(Domain->Stream), Domain->OriginalS2cr);
  Write(D, D->Base + SMR(Domain->Stream), Domain->OriginalSmr);
  Status = Sync(Domain);
  if (CR_ERROR(Status)) {
    return Status;
  }

  Domain->Active = FALSE;
  return CR_SUCCESS;
}
