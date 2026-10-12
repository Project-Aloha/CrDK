# HDK8245 selected panel reference

Original stock XML: https://github.com/Project-Aloha/UEFIFirmwareBackup/blob/main/qcom-hdk8245/BOOT.XF.3.2-00353-SM8250-1/RawFiles/Panel_r66451_fhd_plus_60hz_cmd.xml

Pinned commit b603f9b535325a7ec6b6562f265aa003202aec21. The adjacent XML is byte-exact SHA2568e00f827aa9234a4936ee0893866e4e37e54312f8c0b93d3c8d706ad1a7d2118. The source XML carries no license notice; no replacement license is asserted for that original firmware reference.

The board source FDT is Platforms/KonaCrPkg/Device/qcom-hdk8245/DeviceTreeBlob/Source/hdk8245-runtime.dtb, SHA2562743c213f1c286030eb65647c693cf0e2dfa69ee2a84ceac0e17a86d82de3070. Both chosen bootargs and primary default-panel phandle0x636 select this exact command-mode node. Nearby144Hz/dual and SM8350/CPHY variants are not selected.

Host tests compile unchanged relevant functions from Android SM8250 commit cad09e061ef6cf689a4a1e54d27562e0f042236a, preserving its GPL-2.0-only attribution in generated source. The XML packet list is parsed independently at test time, including its final display-on20ms delay. Native driver registration remains separately gated.
