/** Bounded reader for the HDK8450 panel XML vocabulary. */
#ifndef MDP_PANEL_H_
#define MDP_PANEL_H_

#include <Protocol/EFIMdpDsiProtocol.h>
#include "MdpDsiSequence.h"

typedef struct {
  EFI_MDP_DSI_TIMING Timing;
  UINT32 InterfaceType;
  UINT32 InterfaceColorFormat;
  UINT32 BorderColor;
  UINT32 ControllerCount;
  /* XML cells: assert state, assert us, pre-reset us, post-reset us,
     assert on power-down. Preserve raw values, including zero defaults.
     The HEX_LIST grammar accepts decimal and 0x-prefixed integers. */
  BOOLEAN ResetInfoPresent;
  UINT32 ResetInfo[5];
  /* MDP backlight: none=0, PMIC=1, I2C=2, DSI=3, ACPI=4. */
  BOOLEAN BacklightTypePresent;
  UINT32 BacklightType;
  /* PMIC control: none=0, LPG=1, WLED=2, PWM=3. */
  BOOLEAN BacklightPmicControlTypePresent;
  UINT32 BacklightPmicControlType;
  BOOLEAN BacklightBitWidthPresent;
  UINT32 BacklightBitWidth;
  BOOLEAN Dsc;
  /* Preserve explicit XML compression geometry/version. Private profile ID
     is inventory only; extended caller supplies verified bpc/bpp parameters. */
  UINT32 DscMajor,DscMinor,DscScr,DscProfileId,DscSliceWidth,DscSliceHeight,DscSlicesPerPacket;
  BOOLEAN DscFieldsPresent[7];
  BOOLEAN Cphy;
  BOOLEAN SplitLink;
  BOOLEAN HasCommands;
  BOOLEAN HasUnsupportedCommandMode;
  MDP_DSI_SEQUENCE InitSequence;
  MDP_DSI_SEQUENCE TermSequence;
  BOOLEAN HasUnsupportedGeometry;
  BOOLEAN InvertHsyncPolarity;
  BOOLEAN InvertVsyncPolarity;
  BOOLEAN HsaHseAfterVsVe;
  BOOLEAN DisableEot;
  BOOLEAN HasUnsupportedPhy;
} MDP_PANEL_CONFIG;

EFI_STATUS
MdpParsePanelXml (
  IN CONST CHAR8 *Xml,
  IN UINTN Size,
  OUT MDP_PANEL_CONFIG *Panel
  );

EFI_STATUS
MdpValidateVideoPanel (
  IN CONST MDP_PANEL_CONFIG *Panel
  );

/* Free before replacing/reparsing a successful config. XML may be freed immediately. */
VOID MdpFreePanelConfig (MDP_PANEL_CONFIG *Panel);

#endif
