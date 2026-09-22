/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>

#undef NULL
#include <Library/CranePcieHostLib.h>

int
main (void)
{
  PcieTargetController Controller = {
    .Domain   = 3,
    .BusStart = 4,
    .BusEnd   = 5
  };
  PciePortRuntime Port = {
    .Target     = &Controller,
    .DbiBase    = 0x3F000000ULL,
    .DbiSize    = 0x00000F1DULL,
    .ConfigBase = 0x40000000ULL,
    .ConfigSize = 0x00200000ULL
  };
  UINT64 Address;

  assert (CranePcieHostBuildConfigAddress (
            &Port, 3, 4, 0, 0, 0, 4, &Address) == EFI_SUCCESS);
  assert (Address == 0x3F000000ULL);
  assert (CranePcieHostBuildConfigAddress (
            &Port, 3, 4, 0, 0, 0xF18, 4, &Address) == EFI_SUCCESS);
  assert (Address == 0x3F000F18ULL);
  assert (CranePcieHostBuildConfigAddress (
            &Port, 3, 4, 0, 0, 0xF1D, 1, &Address) == EFI_INVALID_PARAMETER);
  assert (CranePcieHostBuildConfigAddress (
            &Port, 3, 4, 1, 0, 0, 4, &Address) == EFI_NOT_FOUND);
  assert (CranePcieHostBuildConfigAddress (
            &Port, 3, 4, 0, 1, 0, 4, &Address) == EFI_NOT_FOUND);
  assert (CranePcieHostBuildConfigAddress (
            &Port, 3, 5, 0, 0, 0, 4, &Address) == EFI_SUCCESS);
  assert (Address == 0x40100000ULL);
  assert (CranePcieHostBuildConfigAddress (
            &Port, 3, 5, 31, 7, 0xFFC, 4, &Address) == EFI_SUCCESS);
  assert (Address == 0x401FFFFCULL);
  assert (CranePcieHostBuildConfigAddress (
            &Port, 3, 4, 0, 0, 2, 2, &Address) == EFI_SUCCESS);
  assert (Address == 0x3F000002ULL);
  assert (CranePcieHostBuildConfigAddress (
            &Port, 2, 4, 0, 0, 0, 4, &Address) == EFI_INVALID_PARAMETER);
  assert (CranePcieHostBuildConfigAddress (
            &Port, 3, 3, 0, 0, 0, 4, &Address) == EFI_INVALID_PARAMETER);
  assert (CranePcieHostBuildConfigAddress (
            &Port, 3, 4, 32, 0, 0, 4, &Address) == EFI_INVALID_PARAMETER);
  assert (CranePcieHostBuildConfigAddress (
            &Port, 3, 4, 0, 0, 0xFFF, 2, &Address) == EFI_INVALID_PARAMETER);
  assert (CranePcieHostBuildConfigAddress (
            &Port, 3, 4, 0, 0, 0xF19, 1, &Address) == EFI_SUCCESS);
  assert (Address == 0x3F000F19ULL);

  {
    PcieTargetClock Clock = {
      .Name       = "pipe",
      .Controller = "gcc",
      .Id         = "gcc_pcie_0_pipe_clk",
      .RateHz     = 250000000,
      .Provider   = PCIE_CLOCK_PROVIDER_GCC
    };
    assert (CranePcieHostClockProviderValid (&Clock));
    Clock.Controller = "rpmhcc";
    assert (!CranePcieHostClockProviderValid (&Clock));

    Clock.Controller = "pcie0_phy";
    Clock.Id         = "pcie_0_pipe_clk";
    Clock.Provider   = PCIE_CLOCK_PROVIDER_PHY_OUTPUT_ALIAS;
    assert (CranePcieHostClockProviderValid (&Clock));
    Clock.Id = "gcc_pcie_0_pipe_clk";
    assert (!CranePcieHostClockProviderValid (&Clock));

    Clock.Controller = "rpmhcc";
    Clock.Id         = "rpmh_cxo_clk";
    Clock.Provider   = PCIE_CLOCK_PROVIDER_RPMH_ALWAYS_ON;
    assert (CranePcieHostClockProviderValid (&Clock));
    Clock.Id = "gcc_bi_tcxo";
    assert (!CranePcieHostClockProviderValid (&Clock));
  }

  puts ("Crane PCI host bridge: BDF, root-port, aperture and clock-provider checks passed");
  return 0;
}
