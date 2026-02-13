# Crane Development Kit

## Introduce
CrDK is the core implementation of **Project Crane Software Architecture** which provides oskal and os-independent libraries.
This repo is also a UEFI Package and provides uefi drivers.

## Integrate with UEFI
Include `Crane.dsc.inc`, `Crane.fdf.inc` and `Crane.Aprioir.inc` at a correct place of your target uefi package. Then rebuild
your package.

## Integrate with Windows Driver
Open the `Properties manager` of your VS project. Then right click a confiuration in the windows and switch
`Add a exist propery table` in the menu. Choose `CrDK.prop` in the pop up windows.  
Add `Library/XXLib/XX.prop` to your project if you want to use a library in CrDK.
