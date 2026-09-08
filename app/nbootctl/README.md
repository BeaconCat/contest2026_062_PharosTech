# nbootctl

`nbootctl` is the openvela-side client for the KICKPI-K7 N-Boot warm-reset
contract. It reads the handoff published immediately before N-Boot enters
NuttX, or writes a one-shot request before a PSCI warm reset.

```text
nbootctl status
nbootctl verify nuttx a
nbootctl set-active nuttx b
nbootctl mark-successful nuttx b
nbootctl stage nuttx /tmp/nuttx.bin
nbootctl clone nuttx a b
nbootctl update-nboot /tmp/nboot.img
nbootctl reboot console
nbootctl reboot fastboot
nbootctl reboot nuttx-a
nbootctl reboot nuttx-b
```

`status` validates the handoff magic and version and reads the header twice so
a partially updated handoff is rejected. Slot requests affect one boot only;
they do not change the persistent active slot. Reboot requests are read back
before reset. N-Boot consumes and clears every request on the next boot.

`verify`, `set-active`, `mark-successful`, `stage`, and `clone` operate on
either the `nuttx` or `amp` domain. `stage` writes the inactive slot, verifies
its SHA-256 from media, records the new metadata, and activates it. `clone`
copies one verified slot to the other without activating it. Boot-control
mutations update the older redundant copy first, verify it, and then update the
other copy. `update-nboot` replaces the N-Boot FIT on the current medium and
verifies it from media; reboot is left to the caller.
