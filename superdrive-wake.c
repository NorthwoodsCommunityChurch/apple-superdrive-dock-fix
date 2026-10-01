// superdrive-wake: run the Apple USB SuperDrive on any USB port or dock, with SIP and Full
// Security left on.
//
// macOS's own SuperDrive driver (AppleUSBODD) asks the port for 1100 mA and gives up unless the
// port is on Apple's high-power allow-list. Third-party docks and hubs never are. The fix has two
// steps (see README.md):
//   1. (once per restart, as root) Remove AppleUSBODD's matching personalities, so macOS's
//      generic USB storage and DVD drivers take the drive instead:
//          sudo kmutil unload -P -b com.apple.driver.AppleUSBODD
//      kmutil always reports an error for this, because the kernel won't stop Apple's driver
//      instances, but the personalities are still removed.
//   2. (this tool, no root) Send the drive Apple's vendor wake command (0xEA 00 00 00 00 00 01),
//      the same one Linux sends with sg_raw, through the SCSI authoring interface. The drive then
//      spins up and re-enumerates as its real mechanism.
//
// Usage:
//   superdrive-wake                        wake the first SuperDrive found
//   superdrive-wake --location 0x20230000  wake only the SuperDrive on that USB port
//   superdrive-wake --list                 show every SuperDrive, its port and its state
//   --cdb 6|10|12|16                       CDB length for the wake command (default 12)
//
// Build: make   (or: clang -O2 -framework IOKit -framework CoreFoundation -o superdrive-wake superdrive-wake.c)

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/scsi/SCSITaskLib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define APPLE_VID       0x05ac
#define SUPERDRIVE_PID  0x1500
#define APPLE_ODD_CLASS "com_apple_driver_AppleUSBODD"

static int gCdbSize = 12;
static bool gHaveLocation = false;
static uint32_t gLocation = 0;   // with --location: wake only the drive on this USB locationID

static void logmsg(const char *fmt, ...) {
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

// MARK: - Registry lookups

// Reads a number from this entry or the nearest parent that has it (idVendor, locationID, ...).
static bool readNumberUpward(io_service_t svc, CFStringRef key, int64_t *out) {
    CFTypeRef v = IORegistryEntrySearchCFProperty(svc, kIOServicePlane, key, kCFAllocatorDefault,
                                                  kIORegistryIterateRecursively | kIORegistryIterateParents);
    bool ok = v && CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, out);
    if (v) CFRelease(v);
    return ok;
}

static bool isOnSuperDrive(io_service_t svc) {
    int64_t vid = -1, pid = -1;
    return readNumberUpward(svc, CFSTR("idVendor"), &vid) && vid == APPLE_VID &&
           readNumberUpward(svc, CFSTR("idProduct"), &pid) && pid == SUPERDRIVE_PID;
}

static bool locationOf(io_service_t svc, uint32_t *out) {
    int64_t v = 0;
    if (!readNumberUpward(svc, CFSTR("locationID"), &v)) return false;
    *out = (uint32_t)v;
    return true;
}

// Copies one string from the drive's SCSI "Device Characteristics" (Vendor Name, Product Name,
// Product Revision Level) into buf. Returns false if it isn't there.
static bool characteristic(io_service_t svc, CFStringRef key, char *buf, size_t len) {
    CFTypeRef chars = IORegistryEntrySearchCFProperty(svc, kIOServicePlane, CFSTR("Device Characteristics"),
                                                      kCFAllocatorDefault,
                                                      kIORegistryIterateRecursively | kIORegistryIterateParents);
    bool ok = false;
    if (chars && CFGetTypeID(chars) == CFDictionaryGetTypeID()) {
        CFTypeRef s = CFDictionaryGetValue((CFDictionaryRef)chars, key);
        if (s && CFGetTypeID(s) == CFStringGetTypeID())
            ok = CFStringGetCString((CFStringRef)s, buf, (CFIndex)len, kCFStringEncodingUTF8);
    }
    if (chars) CFRelease(chars);
    if (!ok && len) buf[0] = 0;
    return ok;
}

// Asleep, the drive answers INQUIRY as "Apple SuperDrive". Right after a wake it re-enumerates
// and may report its real mechanism (HL-DT-ST) instead. That name proves the drive is awake, but
// the Apple name doesn't prove it's asleep: both of our drives later read discs while reporting
// "Apple SuperDrive". So the wake is skipped only for a drive that reports another vendor.
static bool isAsleep(io_service_t svc) {
    char vendor[64];
    return characteristic(svc, CFSTR("Vendor Name"), vendor, sizeof vendor) && strncmp(vendor, "Apple", 5) == 0;
}

// Every SuperDrive that macOS currently exposes as a disc-authoring device. These exist only
// when the generic DVD driver owns the drive, that is, after step 1.
static io_iterator_t authoringDevices(void) {
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef sub = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(sub, CFSTR(kIOPropertySCSITaskDeviceCategory), CFSTR(kIOPropertySCSITaskAuthoringDevice));
    CFDictionarySetValue(match, CFSTR(kIOPropertyMatchKey), sub);
    CFRelease(sub);
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS) return 0;
    return it;
}

// The SuperDrive to wake: the first one found, or the one on --location.
static io_service_t findAuthoringSuperDrive(void) {
    io_iterator_t it = authoringDevices();
    if (!it) return 0;
    io_service_t svc, found = 0;
    while ((svc = IOIteratorNext(it))) {
        uint32_t loc = 0;
        bool wanted = isOnSuperDrive(svc) && (!gHaveLocation || (locationOf(svc, &loc) && loc == gLocation));
        if (!found && wanted) found = svc;
        else IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    return found;
}

static io_iterator_t superDriveUSBDevices(void) {
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
    int vid = APPLE_VID, pid = SUPERDRIVE_PID;
    CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
    CFNumberRef p = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
    CFDictionarySetValue(match, CFSTR("idVendor"), v);
    CFDictionarySetValue(match, CFSTR("idProduct"), p);
    CFRelease(v);
    CFRelease(p);
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS) return 0;
    return it;
}

static bool appleDriverAttached(io_service_t usbDevice) {
    io_iterator_t it = 0;
    if (IORegistryEntryCreateIterator(usbDevice, kIOServicePlane, kIORegistryIterateRecursively, &it) != KERN_SUCCESS)
        return false;
    bool found = false;
    io_object_t o;
    while ((o = IOIteratorNext(it))) {
        if (IOObjectConformsTo(o, APPLE_ODD_CLASS)) found = true;
        IOObjectRelease(o);
    }
    IOObjectRelease(it);
    return found;
}

// MARK: - Actions

// --list: one line per SuperDrive on USB, with the locationID to pass to --location.
static int listDrives(void) {
    int count = 0;
    io_iterator_t usb = superDriveUSBDevices();
    io_service_t dev;
    while (usb && (dev = IOIteratorNext(usb))) {
        count++;
        uint32_t loc = 0;
        bool haveLoc = locationOf(dev, &loc);
        char serial[64] = "";
        CFTypeRef s = IORegistryEntryCreateCFProperty(dev, CFSTR("USB Serial Number"), kCFAllocatorDefault, 0);
        if (s && CFGetTypeID(s) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)s, serial, sizeof serial, kCFStringEncodingUTF8);
        if (s) CFRelease(s);

        char state[160] = "no DVD device yet: unplug it and plug it back in";
        if (appleDriverAttached(dev)) {
            snprintf(state, sizeof state, "Apple's driver has it: run step 1 (kmutil), then unplug and replug");
        } else {
            io_iterator_t it = authoringDevices();
            io_service_t a;
            while (it && (a = IOIteratorNext(it))) {
                uint32_t aloc = 0;
                if (isOnSuperDrive(a) && locationOf(a, &aloc) && haveLoc && aloc == loc) {
                    char vendor[64], product[64], rev[16];
                    characteristic(a, CFSTR("Vendor Name"), vendor, sizeof vendor);
                    characteristic(a, CFSTR("Product Name"), product, sizeof product);
                    characteristic(a, CFSTR("Product Revision Level"), rev, sizeof rev);
                    snprintf(state, sizeof state, "DVD device ready (%s), reports \"%s %s\" firmware %s",
                             isAsleep(a) ? "awake or asleep" : "awake", vendor, product, rev);
                }
                IOObjectRelease(a);
            }
            if (it) IOObjectRelease(it);
        }
        if (haveLoc) printf("SuperDrive  --location 0x%08x  serial %-14s  %s\n", loc, serial[0] ? serial : "?", state);
        else printf("SuperDrive  (no locationID)  serial %-14s  %s\n", serial[0] ? serial : "?", state);
        IOObjectRelease(dev);
    }
    if (usb) IOObjectRelease(usb);
    if (count == 0) printf("No Apple USB SuperDrive (USB 05ac:1500) is connected.\n");
    return 0;
}

static int sendWake(io_service_t svc) {
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    IOReturn kr = IOCreatePlugInInterfaceForService(svc, kIOMMCDeviceUserClientTypeID,
                                                    kIOCFPlugInInterfaceID, &plugin, &score);
    if (kr != kIOReturnSuccess || !plugin) {
        logmsg("wake failed: no SCSI plug-in (0x%08x)", kr);
        return 1;
    }

    // Keep the plug-in alive until finished: destroying it early closes the connection and
    // GetSCSITaskDeviceInterface then returns NULL.
    MMCDeviceInterface **mmc = NULL;
    (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOMMCDeviceInterfaceID), (LPVOID *)&mmc);
    SCSITaskDeviceInterface **dev = mmc ? (*mmc)->GetSCSITaskDeviceInterface(mmc) : NULL;
    if (!dev) {
        logmsg("wake failed: SCSI task interface unavailable");
        if (mmc) (*mmc)->Release(mmc);
        IODestroyPlugInInterface(plugin);
        return 1;
    }

    int rc = 1;
    kr = (*dev)->ObtainExclusiveAccess(dev);
    if (kr != kIOReturnSuccess) {
        logmsg("wake waiting: another app has the drive open (0x%08x)", kr);
    } else {
        SCSITaskInterface **task = (*dev)->CreateSCSITask(dev);
        if (task) {
            SCSICommandDescriptorBlock cdb;
            memset(cdb, 0, sizeof cdb);
            cdb[0] = 0xEA;  // Apple vendor-specific opcode
            cdb[6] = 0x01;  // allow spin-up
            SCSI_Sense_Data sense;
            memset(&sense, 0, sizeof sense);
            SCSITaskStatus status = kSCSITaskStatus_No_Status;
            UInt64 moved = 0;
            (*task)->SetCommandDescriptorBlock(task, cdb, (UInt8)gCdbSize);
            (*task)->SetScatterGatherEntries(task, NULL, 0, 0, kSCSIDataTransfer_NoDataTransfer);
            (*task)->SetTimeoutDuration(task, 10000);
            kr = (*task)->ExecuteTaskSync(task, &sense, &status, &moved);
            rc = (kr == kIOReturnSuccess && status == kSCSITaskStatus_GOOD) ? 0 : 1;
            if (rc == 0) {
                logmsg("woke the SuperDrive");
            } else if (status == kSCSITaskStatus_CHECK_CONDITION) {
                logmsg("wake failed: sense key 0x%x ASC 0x%02x ASCQ 0x%02x", sense.SENSE_KEY & 0x0F,
                       sense.ADDITIONAL_SENSE_CODE, sense.ADDITIONAL_SENSE_CODE_QUALIFIER);
            } else {
                logmsg("wake failed: IOReturn 0x%08x, SCSI status 0x%02x", kr, status);
            }
            (*task)->Release(task);
        }
        (*dev)->ReleaseExclusiveAccess(dev);
    }
    (*dev)->Release(dev);
    (*mmc)->Release(mmc);
    IODestroyPlugInInterface(plugin);
    return rc;
}

// Waits for the drive to appear as a DVD device, then wakes it unless it reports a non-Apple
// mechanism name (proof it's already awake).
static int wakeWhenReady(int waitSeconds) {
    for (int tick = 0; tick <= waitSeconds * 4; tick++) {
        io_service_t svc = findAuthoringSuperDrive();
        if (svc) {
            bool asleep = isAsleep(svc);
            int rc = asleep ? sendWake(svc) : 0;
            IOObjectRelease(svc);
            if (!asleep) logmsg("SuperDrive is already awake");
            if (rc == 0) return 0;
        }
        usleep(250000);
    }
    if (gHaveLocation)
        logmsg("no SuperDrive ready to wake at locationID 0x%08x; run --list to see each drive's port and state", gLocation);
    else
        logmsg("no SuperDrive ready to wake; run --list to see whether Apple's driver still owns it (step 1)");
    return 1;
}

static int usage(void) {
    fprintf(stderr, "usage: superdrive-wake [--location 0x<locationID>] [--cdb 6|10|12|16]\n"
                    "       superdrive-wake --list\n");
    return 2;
}

int main(int argc, char **argv) {
    bool list = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) {
            list = true;
        } else if (!strcmp(argv[i], "--cdb") && i + 1 < argc) {
            gCdbSize = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--location") && i + 1 < argc) {
            char *end = NULL;
            unsigned long v = strtoul(argv[++i], &end, 0);
            if (!end || *end || v == 0 || v > UINT32_MAX) {
                fprintf(stderr, "--location needs a USB locationID such as 0x20230000 (see --list)\n");
                return 2;
            }
            gLocation = (uint32_t)v;
            gHaveLocation = true;
        } else {
            return usage();
        }
    }
    if (gCdbSize != 6 && gCdbSize != 10 && gCdbSize != 12 && gCdbSize != 16) {
        fprintf(stderr, "CDB size must be 6, 10, 12, or 16\n");
        return 2;
    }
    if (list) return listDrives();
    return wakeWhenReady(10);
}
