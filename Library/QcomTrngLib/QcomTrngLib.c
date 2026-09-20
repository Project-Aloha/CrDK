#include <Base.h>
#include <Library/ArmTrngLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CrTargetTrngLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

STATIC
CONST CR_TRNG_CONFIG *
GetTrngConfig (
  VOID
  )
{
  CONST CR_TRNG_CONFIG  *Config;

  Config = CrTargetGetTrngConfig ();
  if ((Config == NULL) || (Config->BaseAddress == 0) || (Config->MmioSize < sizeof (UINT32)) ||
      (Config->DataOutOffset > (Config->MmioSize - sizeof (UINT32))) ||
      (Config->StatusOffset > (Config->MmioSize - sizeof (UINT32))) ||
      (Config->DataAvailableMask == 0) || (Config->MaxEntropyBits == 0) ||
      (Config->MaxEntropyBits > 32) || ((Config->MaxEntropyBits & 7) != 0) ||
      (Config->PollDelayUs == 0))
  {
    return NULL;
  }

  return Config;
}

RETURN_STATUS
EFIAPI
GetArmTrngVersion (
  OUT UINT16  *MajorRevision,
  OUT UINT16  *MinorRevision
  )
{
  if ((MajorRevision == NULL) || (MinorRevision == NULL)) {
    return RETURN_INVALID_PARAMETER;
  }

  if (GetTrngConfig () == NULL) {
    return RETURN_UNSUPPORTED;
  }

  *MajorRevision = 1;
  *MinorRevision = 0;
  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
GetArmTrngUuid (
  OUT GUID  *Guid
  )
{
  return RETURN_UNSUPPORTED;
}

UINTN
EFIAPI
GetArmTrngMaxSupportedEntropyBits (
  VOID
  )
{
  CONST CR_TRNG_CONFIG  *Config;

  Config = GetTrngConfig ();
  return (Config == NULL) ? 0 : Config->MaxEntropyBits;
}

RETURN_STATUS
EFIAPI
GetArmTrngEntropy (
  IN  UINTN  EntropyBits,
  IN  UINTN  BufferSize,
  OUT UINT8  *Buffer
  )
{
  CONST CR_TRNG_CONFIG  *Config;
  UINTN                 Delay;
  UINTN                 Elapsed;
  UINTN                 ByteCount;
  UINT32                RandomWord;

  Config = GetTrngConfig ();
  if (Config == NULL) {
    return RETURN_UNSUPPORTED;
  }

  if ((Buffer == NULL) || (EntropyBits == 0) || (EntropyBits > Config->MaxEntropyBits) ||
      ((EntropyBits & 7) != 0))
  {
    return RETURN_INVALID_PARAMETER;
  }

  ByteCount = EntropyBits / 8;
  if (BufferSize < ByteCount) {
    return RETURN_BAD_BUFFER_SIZE;
  }

  Elapsed = 0;
  while (TRUE) {
    if ((MmioRead32 (Config->BaseAddress + Config->StatusOffset) & Config->DataAvailableMask) != 0) {
      RandomWord = MmioRead32 (Config->BaseAddress + Config->DataOutOffset);
      CopyMem (Buffer, &RandomWord, ByteCount);
      return RETURN_SUCCESS;
    }

    if (Elapsed >= Config->TimeoutUs) {
      return RETURN_NOT_READY;
    }

    Delay = MIN (Config->PollDelayUs, Config->TimeoutUs - Elapsed);
    MicroSecondDelay (Delay);
    Elapsed += Delay;
  }
}