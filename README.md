# Apple USB SuperDrive on a Thunderbolt dock (macOS)

Run Apple's USB SuperDrive from a Thunderbolt dock or USB hub on macOS, with SIP and Full Security left on, and run two drives at once.

On a dock, the SuperDrive normally never shows up: macOS asks for more power than the port offers and gives up. This repo has a two-step fix (one `kmutil` command and a small wake tool), a script that shows which dock ports are cutting power, and what we learned getting two drives to read discs at the same time.

## The problem

You plug the Apple USB SuperDrive into a dock or hub. macOS shows a "needs more power" notice, and the drive doesn't appear in `drutil list` or Finder.

### Why

- macOS's SuperDrive driver, `com.apple.driver.AppleUSBODD`, claims the drive (USB `05ac:1500`, which the USB tree names "MacBook Air SuperDrive") and asks the port for 1100 mA. Before the fix, `ioreg` showed `kUSBFailedRequestedPower = 1100` on the drive.
- Third-party dock and hub ports offer 500 mA. Every port the drives used on our two docks reports `"kUSBWakePortCurrentLimit" = 500`.
- macOS raises a port's limit only for hubs on an allow-list: the `AppleUSBHostMergeProperties` personalities in `AppleUSBHub.kext`. They cover Apple displays and multiport adapters, Bretford carts, an LG USB-C hub, and a few others. Neither of our Thunderbolt docks is on it. To see each entry and the limit it gets:
  ```bash
  plutil -p /System/Library/Extensions/IOUSBHostFamily.kext/Contents/PlugIns/AppleUSBHub.kext/Contents/Info.plist \
    | grep -E '^    "|kUSBWakePortCurrentLimit'
  ```
- You can't raise a port's limit yourself: writing the current limit to the I/O Registry is rejected (`kIOReturnUnsupported`), even as root.

The drive itself runs fine on 500 mA. It's Apple's driver that refuses. With that driver out of the way, macOS's generic USB storage and DVD drivers take the drive, and it needs only a wake command to spin up.

## The fix

### Step 1: remove Apple's driver rule (once per restart)

```bash
sudo kmutil unload -P -b com.apple.driver.AppleUSBODD
```

**It always prints an error** ("Unknown error occurred stopping classes", with code -536870201 or -603946984). The kernel won't stop a running `com.apple` driver instance, but the matching rule is still removed until the next restart. Unplug the drive and plug it back in.

To check that it worked, run `./superdrive-wake --list` (see Step 2). A drive that says "Apple's driver has it" needs this step. Or look at the drive in `ioreg`: the chain under it should read `IOUSBMassStorageDriver` → `IOSCSIPeripheralDeviceType05` → `IODVDServices`, with no `com_apple_driver_AppleUSBODD`.

SIP and Full Security stay on. `kmutil unload` only removes the rule for this boot. It changes nothing on disk.

### Step 2: wake the drive

```bash
make
./superdrive-wake
```

No sudo needed. The tool sends the drive Apple's vendor wake command, `EA 00 00 00 00 00 01`, the same command Linux users send with `sg_raw`. It goes through macOS's SCSI task interface (`SCSITaskLib`). The drive spins up, reconnects, and reads discs.

After the wake, `drutil` may name the drive by its real mechanism, `HL-DT-ST DVDRW GX50N`, instead of `Apple SuperDrive`. Don't rely on the name: both of our drives later read discs while reporting `Apple SuperDrive`. `drutil` also lists the drive as SupportLevel "Unsupported". Reading discs works; burning is untested.

### When to repeat it

| What happened | Run |
|---|---|
| You unplugged the drive, or its port cut power | Step 2 |
| You restarted the Mac | Step 1, then Step 2 |
| A disc doesn't show up | Step 2 for that drive |

**Don't run the wake while an app is reading the drive.** The wake takes exclusive access to the drive for a moment. If another app holds the drive, the tool says "another app has the drive open" and doesn't send the wake.

## Two drives at once

### Wake each drive by its port

The tool wakes the first SuperDrive it finds, which might be the one that's already working. With two drives, list them and wake the one you mean by its USB `locationID`:

```console
$ ./superdrive-wake --list
SuperDrive  --location 0x21100000  serial XXXXXXXXXXX     DVD device ready (awake or asleep), reports "Apple SuperDrive" firmware 2.03
SuperDrive  --location 0x20230000  serial XXXXXXXXXXX     DVD device ready (awake or asleep), reports "Apple SuperDrive" firmware 2.00

$ ./superdrive-wake --location 0x20230000
```

It prints `woke the SuperDrive` when the drive accepts the command.

A `locationID` belongs to the port, not the drive: it stays the same when the drive reconnects on the same port and changes when you move the drive.

### Give each drive its own good port

This was the real problem. With two drives on one dock, each drive dropped off USB a few seconds after we inserted a disc, and came back asleep. It looked like a software bug. It was the dock's ports cutting power.

When a port can't supply what the device draws, the port's over-current protection cuts power. The device disconnects, comes back, and often disconnects again: a drive that "keeps disconnecting" with a disc inside. macOS counts each cut. `port-power-check.sh` shows the count for every port in use:

```console
$ ./port-power-check.sh
LOCATION      LIMIT  CONNECTS  POWER-CUTS  DEVICE
0x20212300    500mA        25          14
0x20213000    500mA       107          62  Stream Deck MK.2
0x20230000    500mA         4           0  MacBook Air SuperDrive
0x21100000    500mA         2           0  MacBook Air SuperDrive
```

(Trimmed. Counters reset when the Mac restarts. A USB-A socket appears twice, once for its USB 2 half and once for its USB 3 half, with the same count.)

What we found on a Belkin Thunderbolt 3 Dock Pro:

| Port | Power cuts during one day of ripping |
|---|---|
| Front USB-A (`0x20230000`) | 0 |
| Rear USB-A (`0x20213000`) | 62 |
| Rear USB-A (`0x20212300`) | 14 |

In the USB tree, the front port hangs off the dock's first internal hub; the rear ports sit one and two hubs further down.

- A USB extension cable made the power cuts more frequent.
- Our two drives differed. The one with firmware 2.00 ran reliably only on the Belkin's front port. The one with firmware 2.03 also ran on a second dock.
- The setup that worked: one drive on the Belkin's front USB-A port, the other on a separate Elgato Thunderbolt 3 Dock (daisy-chained from the Belkin), each plugged in directly with no extension cable. Both drives read discs at the same time with zero power cuts.
- After a power cut the drive comes back asleep. Run the wake for that drive again.

The 62 on `0x20213000` stayed after we moved the drive away. A Stream Deck uses that port now.

## What we tried (and why it failed)

| Attempt | Result |
|---|---|
| Raise the port's current limit in the I/O Registry | Rejected with `kIOReturnUnsupported`, even as root. |
| Two drives on the Belkin's rear ports | Each drive dropped a few seconds after a disc went in. The power-cut counters showed why: 62 and 14 cuts. |
| Extension cables to reach a better port | More power cuts, not fewer. |
| Waking "the first SuperDrive found" with two drives | It can wake the drive that's already working and leave the other asleep. Fixed with `--location`. |
| Judging asleep or awake by the drive's name | Unreliable. Working drives can report `Apple SuperDrive`. |

## Files

```
apple-superdrive-dock-fix/
├── superdrive-wake.c     the wake tool (C, IOKit SCSITaskLib)
├── port-power-check.sh   per-port power limit and power-cut counter
├── Makefile              builds superdrive-wake
├── README.md
├── CREDITS.md
└── LICENSE
```

## Building

```bash
git clone https://github.com/NorthwoodsCommunityChurch/apple-superdrive-dock-fix.git
cd apple-superdrive-dock-fix
make
```

You need the Xcode Command Line Tools (`xcode-select --install`). The build is one `clang` call against IOKit and CoreFoundation, with no other dependencies.

## Compatibility

Tested on:
- **Mac**: MacBook Pro (Mac15,11), Apple silicon
- **macOS**: 26.5.2 (25F84), SIP on, Full Security
- **Drives**: two Apple USB SuperDrives (USB `05ac:1500`), firmware 2.00 and 2.03
- **Docks**: Belkin Thunderbolt 3 Dock Pro; Elgato Thunderbolt 3 Dock

Not tested: Intel Macs, other macOS versions, burning discs, or starting the fix automatically at login.

## License

MIT License. Copyright (c) 2026 Northwoods Community Church. See [LICENSE](LICENSE).

## Credits

See [CREDITS.md](CREDITS.md). The wake command comes from the Linux community's SuperDrive notes.
