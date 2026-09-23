/** @file
 *  ABI declarations for the Qualcomm MU QUP/GENI protocols.
 *
 *  The Waipio image contains prebuilt MU I2C, SPI, and GPI drivers.  Those
 *  drivers do not publish the PI I2C/SPI protocols; they publish the private
 *  Qualcomm interfaces below.  Keep these declarations local to CranePkg so
 *  the adapter drivers do not take ownership of the QUP registers.
 *
 *  Function tables, data layouts, and GUIDs follow the Qualcomm EFII2C.h,
 *  EFISPI.h, and EFIGpiProtocol.h interfaces and the Waipio driver tables.
 *  Status enums can change without a protocol revision change.  Only the
 *  shared status values used by the adapters are declared here; other native
 *  errors must not be interpreted using an older firmware's enum.
 */
#ifndef __EFI_MU_BUS_PROTOCOL_H__
#define __EFI_MU_BUS_PROTOCOL_H__

#include <Uefi.h>

/* Qualcomm private protocol GUIDs. */
#define EFI_MU_I2C_PROTOCOL_GUID \
  { 0xb27ae8b1, 0x3e10, 0x4d07, { 0xab, 0x5c, 0xeb, 0x9a, 0x6d, 0xc6, 0xfa, 0x8f } }
#define EFI_MU_SPI_PROTOCOL_GUID \
  { 0x4c7ffd28, 0x6a06, 0x4425, { 0x9e, 0xe2, 0x67, 0x6e, 0xbc, 0x08, 0x96, 0x83 } }
#define EFI_MU_GPI_INIT_PROTOCOL_GUID \
  { 0x569ea0de, 0xb557, 0x4043, { 0x84, 0xcf, 0x01, 0x10, 0x3f, 0xe5, 0x16, 0xe5 } }

extern EFI_GUID gQcomI2CProtocolGuid;
extern EFI_GUID gQcomSPIProtocolGuid;
extern EFI_GUID gQcomGpiInitProtocolGuid;

/* MU I2C API (i2c_api.h / EFII2C.h). */
#define MU_I2C_PROTOCOL_REVISION  0x0000000000010000ULL

typedef UINT32 MU_I2C_INSTANCE;
enum {
  MU_I2C_INSTANCE_001 = 1,
  MU_I2C_INSTANCE_MAX = 25
};

typedef UINT32 MU_I2C_STATUS;
#define MU_I2C_SUCCESS                              0U
#define MU_I2C_ERROR_INVALID_PARAMETER              1U
#define MU_I2C_ERROR_UNSUPPORTED_INSTANCE            2U
#define MU_I2C_ERROR_API_INVALID_EXECUTION_LEVEL    3U
#define MU_I2C_ERROR_API_NOT_SUPPORTED              4U
#define MU_I2C_ERROR_API_ASYNC_MODE_NOT_SUPPORTED   5U
#define MU_I2C_ERROR_API_PROTOCOL_MODE_NOT_SUPPORTED 6U
#define MU_I2C_ERROR_HANDLE_ALLOCATION              7U
#define MU_I2C_ERROR_HW_INFO_ALLOCATION             8U
#define MU_I2C_ERROR_BUS_NOT_IDLE                   9U
#define MU_I2C_ERROR_TRANSFER_TIMEOUT              10U
#define MU_I2C_ERROR_MEM_ALLOC_FAIL                11U

typedef enum {
  MU_I2C_MODE_I2C = 0,
  MU_I2C_MODE_SMBUS,
  MU_I2C_MODE_I3C_SDR,
  MU_I2C_MODE_I3C_HDR_DDR,
  MU_I2C_MODE_I3C_BROADCAST_CCC,
  MU_I2C_MODE_I3C_DIRECT_CCC,
  MU_I2C_MODE_I3C_IBI_READ
} MU_I2C_MODE;

typedef struct {
  UINT32        BusFrequencyKHz;
  UINT32        SlaveAddress;
  MU_I2C_MODE   Mode;
  UINT32        SlaveMaxClockStretchUs;
  UINT32        CoreConfiguration1;
  UINT32        CoreConfiguration2;
} MU_I2C_SLAVE_CONFIG;

typedef struct {
  UINT8   *Buffer;
  UINT32   Length;
  UINT32   Flags;
} MU_I2C_DESCRIPTOR;

#define MU_I2C_FLAG_START  0x00000001U
#define MU_I2C_FLAG_STOP   0x00000002U
#define MU_I2C_FLAG_WRITE  0x00000004U
#define MU_I2C_FLAG_READ   0x00000008U

typedef VOID (*MU_I2C_CALLBACK)(UINT32 TransferStatus, UINT32 Transferred,
                                VOID *Context);

typedef MU_I2C_STATUS (EFIAPI *MU_I2C_OPEN)(
    MU_I2C_INSTANCE Instance, VOID **I2cHandle);
typedef MU_I2C_STATUS (EFIAPI *MU_I2C_READ)(
    VOID *I2cHandle, MU_I2C_SLAVE_CONFIG *Config, UINT16 Offset,
    UINT16 OffsetLength, UINT8 *Buffer, UINT16 BufferLength,
    UINT32 *Read, UINT32 TimeoutMs);
typedef MU_I2C_STATUS (EFIAPI *MU_I2C_WRITE)(
    VOID *I2cHandle, MU_I2C_SLAVE_CONFIG *Config, UINT16 Offset,
    UINT16 OffsetLength, UINT8 *Buffer, UINT16 BufferLength,
    UINT32 *Written, UINT32 TimeoutMs);
typedef MU_I2C_STATUS (EFIAPI *MU_I2C_TRANSFER)(
    VOID *I2cHandle, MU_I2C_SLAVE_CONFIG *Config,
    MU_I2C_DESCRIPTOR *Descriptors, UINT16 DescriptorCount,
    MU_I2C_CALLBACK Callback, VOID *Context, UINT32 Delay,
    UINT32 *Transferred);
typedef MU_I2C_STATUS (EFIAPI *MU_I2C_CLOSE)(VOID *I2cHandle);

typedef struct _MU_I2C_PROTOCOL {
  UINT64            Revision;
  MU_I2C_OPEN       Open;
  MU_I2C_READ       Read;
  MU_I2C_WRITE      Write;
  MU_I2C_TRANSFER   Transfer;
  MU_I2C_CLOSE      Close;
} MU_I2C_PROTOCOL;

/* MU SPI API (SpiApi.h / SpiDevice.h / EFISPI.h). */
#define MU_SPI_PROTOCOL_REVISION  0x0000000000010000ULL

typedef UINT32 MU_SPI_INSTANCE;
enum {
  MU_SPI_INSTANCE_001 = 0,
  MU_SPI_INSTANCE_MAX  = 20
};

typedef UINT32 MU_SPI_STATUS;
#define MU_SPI_SUCCESS                              0U
#define MU_SPI_ERROR_INVALID_PARAM                    2U
#define MU_SPI_ERROR_HW_INFO_ALLOCATION               3U
#define MU_SPI_ERROR_MEM_ALLOC                        4U
#define MU_SPI_ERROR_HANDLE_ALLOCATION                6U
#define MU_SPI_ERROR_UNSUPPORTED_IN_ISLAND_MODE       8U
#define MU_SPI_TRANSFER_FORCE_TERMINATED             17U
#define MU_SPI_ERROR_TRANSFER_TIMEOUT                21U
#define MU_SPI_ERROR_PENDING_TRANSFER                22U

/* Limits enforced by the MU SPILib validation path. */
#define MU_SPI_MAX_SLAVE_NUMBER  3U
#define MU_SPI_MIN_FRAME_BITS    8U
#define MU_SPI_MAX_FRAME_BITS    32U

typedef enum {
  MU_SPI_CLK_NORMAL = 0,
  MU_SPI_CLK_ALWAYS_ON
} MU_SPI_CLOCK_MODE;
typedef enum {
  MU_SPI_CLK_IDLE_LOW = 0,
  MU_SPI_CLK_IDLE_HIGH
} MU_SPI_CLOCK_POLARITY;
typedef enum {
  MU_SPI_INPUT_FIRST_MODE = 0,
  MU_SPI_OUTPUT_FIRST_MODE
} MU_SPI_SHIFT_MODE;
typedef enum {
  MU_SPI_CS_ACTIVE_LOW = 0,
  MU_SPI_CS_ACTIVE_HIGH
} MU_SPI_CS_POLARITY;
typedef enum {
  MU_SPI_CS_DEASSERT = 0,
  MU_SPI_CS_KEEP_ASSERTED
} MU_SPI_CS_MODE;
typedef enum {
  MU_SPI_CORE_MODE_SLAVE = 0,
  MU_SPI_CORE_MODE_MASTER
} MU_SPI_CORE_MODE;
typedef enum {
  MU_SPI_LOOPBACK_DISABLED = 0,
  MU_SPI_LOOPBACK_ENABLED
} MU_SPI_LOOPBACK_MODE;

typedef struct {
  MU_SPI_CLOCK_MODE       ClockMode;
  MU_SPI_CLOCK_POLARITY   ClockPolarity;
  MU_SPI_SHIFT_MODE       ShiftMode;
  UINT32                  DeassertionTime;
  UINT32                  MinSlaveFrequencyHz;
  UINT32                  MaxSlaveFrequencyHz;
  MU_SPI_CS_POLARITY      CsPolarity;
  MU_SPI_CS_MODE           CsMode;
  BOOLEAN                 HsMode;
} MU_SPI_DEVICE_PARAMETERS;

typedef struct {
  UINT32             SlaveNumber;
  MU_SPI_CORE_MODE   CoreMode;
} MU_SPI_BOARD_INFO;

typedef struct {
  UINT32                  NumBits;
  MU_SPI_LOOPBACK_MODE    LoopbackMode;
} MU_SPI_TRANSFER_PARAMETERS;

typedef struct {
  MU_SPI_DEVICE_PARAMETERS   DeviceParameters;
  MU_SPI_BOARD_INFO          BoardInfo;
  MU_SPI_TRANSFER_PARAMETERS TransferParameters;
} MU_SPI_DEVICE_INFO;

typedef MU_SPI_STATUS (EFIAPI *MU_SPI_OPEN)(
    MU_SPI_INSTANCE Instance, VOID **SpiHandle);
typedef MU_SPI_STATUS (EFIAPI *MU_SPI_TRANSFER)(
    VOID *SpiHandle, MU_SPI_DEVICE_INFO *DeviceInfo,
    CONST UINT8 *WriteBuffer, UINT32 WriteLength,
    UINT8 *ReadBuffer, UINT32 ReadLength);
typedef MU_SPI_STATUS (EFIAPI *MU_SPI_CLOSE)(VOID *SpiHandle);

typedef struct _MU_SPI_PROTOCOL {
  UINT64             Revision;
  MU_SPI_OPEN        Open;
  MU_SPI_TRANSFER    Transfer;
  MU_SPI_CLOSE       Close;
} MU_SPI_PROTOCOL;

/*
 * MU GPI API (EFIGpiProtocol.h).  The payload structures are intentionally
 * opaque: Crane only forwards the protocol pointer and does not fabricate the
 * private gpi.h object layouts.
 */
#define MU_GPI_PROTOCOL_REVISION  0x0000000000010002ULL

typedef VOID *MU_GPI_CLIENT_HANDLE;
typedef struct _MU_GPI_IFACE_PARAMS MU_GPI_IFACE_PARAMS;
typedef struct _MU_GPI_CHAN_STATUS  MU_GPI_CHAN_STATUS;
typedef struct _MU_GPI_TRE_REQUEST  MU_GPI_TRE_REQUEST;
typedef struct _MU_GPI_DEBUG_REGS   MU_GPI_DEBUG_REGS;

typedef EFI_STATUS (EFIAPI *MU_GPI_IFACE_REG)(MU_GPI_IFACE_PARAMS *Params);
typedef EFI_STATUS (EFIAPI *MU_GPI_REG_SAVE)(MU_GPI_CLIENT_HANDLE Handle,
                                              MU_GPI_DEBUG_REGS *Regs);
typedef EFI_STATUS (EFIAPI *MU_GPI_QUERY_CHAN_STATUS)(
    MU_GPI_CLIENT_HANDLE Handle, UINT32 Channel, MU_GPI_CHAN_STATUS *Status);
typedef EFI_STATUS (EFIAPI *MU_GPI_IFACE_POLL)(MU_GPI_CLIENT_HANDLE Handle);
typedef EFI_STATUS (EFIAPI *MU_GPI_ISSUE_CMD)(
    MU_GPI_CLIENT_HANDLE Handle, UINT32 Channel, UINT32 Command,
    UINT32 UserCommand, VOID *UserData);
typedef EFI_STATUS (EFIAPI *MU_GPI_PROCESS_TRE)(MU_GPI_TRE_REQUEST *Request);
typedef EFI_STATUS (EFIAPI *MU_GPI_IFACE_DEREG)(MU_GPI_CLIENT_HANDLE Handle);
typedef EFI_STATUS (EFIAPI *MU_GPI_IFACE_ACTIVE)(
    MU_GPI_CLIENT_HANDLE Handle, BOOLEAN Active);

typedef struct _MU_GPI_PROTOCOL {
  UINT64                    Revision;
  MU_GPI_IFACE_REG          IfaceReg;
  MU_GPI_REG_SAVE           RegSave;
  MU_GPI_QUERY_CHAN_STATUS  QueryChanStatus;
  MU_GPI_IFACE_POLL         IfacePoll;
  MU_GPI_ISSUE_CMD          IssueCmd;
  MU_GPI_PROCESS_TRE        ProcessTre;
  MU_GPI_IFACE_DEREG        IfaceDereg;
  MU_GPI_IFACE_ACTIVE       IfaceActive;
} MU_GPI_PROTOCOL;

/* These private structures cross the boundary into prebuilt firmware. */
STATIC_ASSERT (sizeof (MU_I2C_SLAVE_CONFIG) == 24, "MU I2C configuration ABI");
STATIC_ASSERT (OFFSET_OF (MU_I2C_SLAVE_CONFIG, Mode) == 8,
               "MU I2C mode offset");
STATIC_ASSERT (OFFSET_OF (MU_I2C_DESCRIPTOR, Length) == sizeof (VOID *),
               "MU I2C descriptor length offset");
STATIC_ASSERT (OFFSET_OF (MU_I2C_DESCRIPTOR, Flags) == sizeof (VOID *) + 4,
               "MU I2C descriptor flags offset");
STATIC_ASSERT (OFFSET_OF (MU_I2C_PROTOCOL, Transfer) == 8 + 3 * sizeof (VOID *),
               "MU I2C transfer slot");
STATIC_ASSERT (OFFSET_OF (MU_I2C_PROTOCOL, Close) == 8 + 4 * sizeof (VOID *),
               "MU I2C close slot");
STATIC_ASSERT (sizeof (MU_SPI_DEVICE_PARAMETERS) == 36,
               "MU SPI device parameters ABI");
STATIC_ASSERT (OFFSET_OF (MU_SPI_DEVICE_INFO, BoardInfo) == 36,
               "MU SPI board information offset");
STATIC_ASSERT (OFFSET_OF (MU_SPI_DEVICE_INFO, TransferParameters) == 44,
               "MU SPI transfer parameters offset");
STATIC_ASSERT (sizeof (MU_SPI_DEVICE_INFO) == 52, "MU SPI device information ABI");
STATIC_ASSERT (OFFSET_OF (MU_SPI_PROTOCOL, Transfer) == 8 + sizeof (VOID *),
               "MU SPI transfer slot");
STATIC_ASSERT (OFFSET_OF (MU_GPI_PROTOCOL, IfaceActive) == 8 + 7 * sizeof (VOID *),
               "MU GPI active slot");

#endif /* __EFI_MU_BUS_PROTOCOL_H__ */
