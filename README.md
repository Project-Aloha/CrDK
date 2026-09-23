# Crane Development Kit
## Introduce
CrDK is the core implementation of [**Project Crane Software Architecture**](https://aloha.firmware.icu/Introduction/CrDK.html) which provides oskal and os-independent libraries.
This repo is also a UEFI Package and provides uefi drivers.

## Integrate with UEFI
Include `Crane.dsc.inc`, `Crane.fdf.inc` and `Crane.Apriori.inc` at a correct place of your target uefi package. Then rebuild
your package.  
`CrDALDxe` publishes silicon descriptions from the selected `CrTargetLib`
provider. DXE consumers use `CrDalLib`; portable hardware libraries receive
their target context from the DXE resource owner. See
[CrDAL.md](Documentation/CrDAL.md) for the device-data ABI and ownership rules.
The [Waipio bring-up notes](Documentation/WaipioBringup.md) describe resource
ordering, MU bus integration, source provenance, and validation limits.

## Integrate with Windows Driver
Open the `Properties manager` of your VS project. Then right click a confiuration in the windows and switch
`Add a exist propery table` in the menu. Choose `CrDK.prop` in the pop up windows. Add `Library/XXLib/XX.prop` to your
project if you want to use a library in CrDK.  
**Crane Windows Development Kit** contains our windows driver implementation with CrDK.
