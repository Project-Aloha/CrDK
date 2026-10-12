# ESP variable persistence

HDK8450 binds this library to the standard DXE variable service. It restores
the complete authenticated NV store before parsing, quota accounting and
authentication initialization. Authentication and variable-policy checks remain
in the standard service; volatile variables are not included in the files.

At boot, the backend connects UFS and SD storage and considers mounted GPT EFI
System Partitions. Internal media takes precedence over removable media; device
path and partition GUID determine a stable tie break. The selected partition
and media identity remain fixed for the session. It writes these files there:

```
\EFI\Crane\NvVars0.bin
\EFI\Crane\NvVars1.bin
```

Each completed firmware-time variable operation saves a changed store before
returning success. Two generations, CRCs, exact sizes and structural validation
allow fallback when the newer snapshot is incomplete or corrupt. Writes go to
the inactive file; its valid header is published only after flushing the full
payload. Short writes, flush failures and media changes return errors. A failed
flush or close can have reached storage despite the error, so a later boot may
recover that valid generation; the previous acknowledged file is preserved.

If no ESP is present during initialization, variables remain in memory for that
boot and are discarded on restart. The library does not create or format a
partition, choose an arbitrary FAT filesystem, or adopt a different disk later.

After ExitBootServices, writes retain the existing emulated RAM behavior and
are not saved to the ESP. No boot filesystem or block protocol is called at OS
runtime. Checksums detect incomplete data, but a writable ESP is not a protected
Secure Boot or rollback-resistant storage root.

For a device test, create an NV variable using a private test GUID:

```
setvar CrNvTest -guid 736540de-00c1-466a-929b-f88e26f6e1a4 -nv -bs -rt =01020304
```

Reboot, read it, then delete it and reboot again:

```
setvar CrNvTest -guid 736540de-00c1-466a-929b-f88e26f6e1a4
setvar CrNvTest -guid 736540de-00c1-466a-929b-f88e26f6e1a4 =
```

Confirm the
serial log reports `ESP variables: restored generation` and that deletion also
survives. Repeat without an ESP to confirm the variable disappears on reboot.
Physical reboot and power-loss behavior must be checked on the device.
