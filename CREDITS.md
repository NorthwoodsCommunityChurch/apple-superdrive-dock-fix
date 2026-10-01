# Credits

## Prior art

| Source | What it gave us |
|---|---|
| [udev rules to use Apple SuperDrive on Linux](https://gist.github.com/yookoala/818c1ff057e3d965980b7fd3bf8f77a6) (yookoala) | The SuperDrive wake command, `EA 00 00 00 00 00 01`, sent with `sg_raw` on Linux |
| [sg3_utils](https://sg.danny.cz/sg/sg3_utils.html) | `sg_raw`, the Linux tool those notes use to send the command |

## Frameworks

| Name | Description | License |
|---|---|---|
| IOKit (`SCSITaskLib`) | Apple's interface for sending SCSI commands to the drive from user space | Apple system framework |
| CoreFoundation | Apple system framework | Apple system framework |

No third-party code or libraries. `superdrive-wake` builds with `clang` from the Xcode Command Line Tools.
