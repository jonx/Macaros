/* cocoametal_media.m — the host side of "which physical media may AROS see".
 *
 * Enumeration and hotplug come from DiskArbitration; the host's own disk is
 * never offered. A grant unmounts the volume on the macOS side, sets the device
 * node's permissions to match the access the user chose, writes an AROS mount
 * description into the shared directory, and records the grant in
 * aros-host.conf. MediaWatch, inside AROS, mounts what appears there.
 *
 * Independent work: written from Apple's DiskArbitration/Foundation
 * documentation [PUB] and this project's own mount-description format [OURS].
 * No third-party implementation was read or consulted.
 */
#import <Foundation/Foundation.h>
#import <DiskArbitration/DiskArbitration.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>
#include <string.h>

#include "cocoametal_media.h"

/* filesystem -> the AROS handler and the mount DosType it is described with.
 * The handler reads the real variant off the volume; for the FAT family the
 * DosType is informational. Keep in step with graft/macaros-media. */
static NSArray *handler_for(NSString *fs) {
    static NSDictionary *H = nil;
    if (!H) H = @{
        @"exfat": @[@"exfat-handler", @"0x46415458"],
        @"msdos": @[@"fat-handler",   @"0x46415400"],
        @"fat32": @[@"fat-handler",   @"0x46415402"],
    };
    return H[fs ?: @""];
}

/* ---- where the two artifacts live ----------------------------------------
 * The mount descriptions go in a directory both sides can reach: the host share
 * that AROS mounts as MacRW:. A release .app is signed and sealed, so nothing
 * may be written inside the bundle's own AROS tree. */
static NSString *share_path(void) {
    const char *env = getenv("MACAROS_MEDIA_DIR");
    if (env && *env) return @(env);
    const char *vol = getenv("AROS_HOST_VOLUME");
    if (vol && *vol) {
        for (NSString *line in [@(vol) componentsSeparatedByString:@"\n"]) {
            if (![line hasPrefix:@"MacRW:"]) continue;
            NSString *path = [line substringFromIndex:6];
            NSRange semi = [path rangeOfString:@";"];
            if (semi.location != NSNotFound) path = [path substringToIndex:semi.location];
            if (path.length)
                return [path stringByAppendingPathComponent:@".macaros-media"];
        }
    }
    return [NSHomeDirectory() stringByAppendingPathComponent:@"AROS/Shared/.macaros-media"];
}

const char *cm_media_dir(void) {
    static char buf[1024];
    @autoreleasepool {
        snprintf(buf, sizeof buf, "%s", share_path().UTF8String ?: "");
    }
    return buf;
}

static NSString *conf_path(void) {
    const char *env = getenv("AROS_HOST_CONF");
    if (env && *env) return @(env);
    NSArray *as = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,
                                                      NSUserDomainMask, YES);
    NSString *base = as.firstObject ?: NSHomeDirectory();
    return [base stringByAppendingPathComponent:@"AROS/aros-host.conf"];
}

/* ---- grants in aros-host.conf ---------------------------------------------
 * One line per grant: "media <name> <ro|rw> <fs> <identity>". Every other line
 * of the file belongs to the settings schema and is preserved untouched. */
@interface CMGrant : NSObject
@property (nonatomic, copy) NSString *name, *mode, *fs, *ident;
@end
@implementation CMGrant @end

static NSArray<CMGrant *> *read_grants(NSMutableArray<NSString *> *others) {
    NSMutableArray *grants = [NSMutableArray array];
    NSString *text = [NSString stringWithContentsOfFile:conf_path()
                                               encoding:NSUTF8StringEncoding error:NULL];
    for (NSString *line in [(text ?: @"") componentsSeparatedByString:@"\n"]) {
        NSMutableArray *f = [NSMutableArray array];
        for (NSString *w in [line componentsSeparatedByCharactersInSet:
                                 [NSCharacterSet whitespaceCharacterSet]])
            if (w.length) [f addObject:w];
        if (f.count >= 5 && [f[0] isEqualToString:@"media"]) {
            CMGrant *g = [CMGrant new];
            g.name = f[1]; g.mode = f[2]; g.fs = f[3];
            g.ident = [[f subarrayWithRange:NSMakeRange(4, f.count - 4)]
                          componentsJoinedByString:@" "];
            [grants addObject:g];
        } else {
            [others addObject:line];
        }
    }
    return grants;
}

static void write_grants(NSArray<CMGrant *> *grants, NSArray<NSString *> *others) {
    NSMutableString *out = [NSMutableString string];
    NSArray *kept = others;
    while (kept.count && ![kept.lastObject length])     /* one trailing newline, not a growing tail */
        kept = [kept subarrayWithRange:NSMakeRange(0, kept.count - 1)];
    for (NSString *line in kept)
        [out appendFormat:@"%@\n", line];
    for (CMGrant *g in grants)
        [out appendFormat:@"media %@ %@ %@ %@\n", g.name, g.mode, g.fs, g.ident];
    NSString *path = conf_path();
    [[NSFileManager defaultManager] createDirectoryAtPath:[path stringByDeletingLastPathComponent]
                             withIntermediateDirectories:YES attributes:nil error:NULL];
    [out writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:NULL];
}

/* ---- the medium itself ----------------------------------------------------- */
static NSString *fs_of(NSDictionary *desc) {
    NSString *kind = desc[(__bridge NSString *)kDADiskDescriptionVolumeKindKey];
    kind = kind.lowercaseString;
    if (!kind.length) return @"";
    if ([kind containsString:@"exfat"]) return @"exfat";
    if ([kind containsString:@"fat32"]) return @"fat32";
    if ([kind containsString:@"fat"] || [kind containsString:@"dos"]) return @"msdos";
    return kind;
}

static NSString *uuid_string(CFUUIDRef uuid) {
    if (!uuid) return nil;
    CFStringRef s = CFUUIDCreateString(kCFAllocatorDefault, uuid);
    NSString *out = (__bridge_transfer NSString *)s;
    return out;
}

/* The key a grant is remembered by. Identical in graft/macaros-media, so a
 * medium granted from either half is recognised by the other. */
static NSString *identity_of(NSDictionary *desc) {
    NSString *u = uuid_string((__bridge CFUUIDRef)desc[(__bridge NSString *)kDADiskDescriptionVolumeUUIDKey]);
    if (u.length) return [@"volumeuuid:" stringByAppendingString:u];
    u = uuid_string((__bridge CFUUIDRef)desc[(__bridge NSString *)kDADiskDescriptionMediaUUIDKey]);
    if (u.length) return [@"diskuuid:" stringByAppendingString:u];
    NSString *name = desc[(__bridge NSString *)kDADiskDescriptionVolumeNameKey] ?: @"-";
    NSNumber *size = desc[(__bridge NSString *)kDADiskDescriptionMediaSizeKey] ?: @0;
    return [NSString stringWithFormat:@"shape:%@/%@/%@", name, size, fs_of(desc)];
}

static DASessionRef session(void) {
    static DASessionRef s = NULL;
    if (!s) s = DASessionCreate(kCFAllocatorDefault);
    return s;
}

/* Kept apart from session(): the hotplug session stays scheduled on the run
 * loop, while the one above is scheduled only for the length of one operation. */
static DASessionRef watch_session(void) {
    static DASessionRef s = NULL;
    if (!s) s = DASessionCreate(kCFAllocatorDefault);
    return s;
}

/* Every /dev/disk node, described. DiskArbitration answers for a BSD name
 * without a run loop, so a snapshot needs no callbacks. */
static NSArray<NSDictionary *> *attached_media(void) {
    NSMutableArray *found = [NSMutableArray array];
    DIR *dev = opendir("/dev");
    if (!dev) return found;
    struct dirent *entry;
    while ((entry = readdir(dev)) != NULL) {
        if (strncmp(entry->d_name, "disk", 4) != 0) continue;
        DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session(), entry->d_name);
        if (!disk) continue;
        NSDictionary *desc = (__bridge_transfer NSDictionary *)DADiskCopyDescription(disk);
        CFRelease(disk);
        if (!desc) continue;
        NSNumber *internal = desc[(__bridge NSString *)kDADiskDescriptionDeviceInternalKey];
        NSNumber *whole    = desc[(__bridge NSString *)kDADiskDescriptionMediaWholeKey];
        NSNumber *leaf     = desc[(__bridge NSString *)kDADiskDescriptionMediaLeafKey];
        if (internal.boolValue) continue;              /* the Mac's own disk is never on offer */
        if (whole.boolValue && !leaf.boolValue) continue;   /* offer partitions, not the container */
        if (!fs_of(desc).length) continue;             /* nothing a filesystem lives on */
        [found addObject:desc];
    }
    closedir(dev);
    [found sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
        return [a[(__bridge NSString *)kDADiskDescriptionMediaBSDNameKey]
                compare:b[(__bridge NSString *)kDADiskDescriptionMediaBSDNameKey]];
    }];
    return found;
}

static NSString *bsd_of(NSDictionary *desc) {
    return desc[(__bridge NSString *)kDADiskDescriptionMediaBSDNameKey];
}

static NSString *mount_point(NSDictionary *desc) {
    NSURL *url = desc[(__bridge NSString *)kDADiskDescriptionVolumePathKey];
    return url.path;
}

/* ---- unmount / remount: DiskArbitration, pumped to completion --------------- */
typedef struct { int done; int ok; char err[160]; } CMDAWait;

static void da_done(DADiskRef disk, DADissenterRef dissenter, void *ctx) {
    (void)disk;
    CMDAWait *w = ctx;
    w->ok = (dissenter == NULL);
    if (dissenter) {
        NSString *why = (__bridge NSString *)DADissenterGetStatusString(dissenter);
        snprintf(w->err, sizeof w->err, "%s",
                 why.UTF8String ?: "the Mac would not release the volume");
    }
    w->done = 1;
}

static int pump_until(CMDAWait *w, double seconds) {
    CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + seconds;
    while (!w->done && CFAbsoluteTimeGetCurrent() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    return w->done && w->ok;
}

static int detach_from_host(NSDictionary *desc, char *err, int errlen) {
    if (!mount_point(desc)) return 1;                  /* already ours to use */
    DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session(),
                                             bsd_of(desc).UTF8String);
    if (!disk) { snprintf(err, errlen, "cannot address %s", bsd_of(desc).UTF8String); return 0; }
    CMDAWait wait = {0, 0, ""};
    DASessionScheduleWithRunLoop(session(), CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    DADiskUnmount(disk, kDADiskUnmountOptionDefault, da_done, &wait);
    int ok = pump_until(&wait, 8.0);
    DASessionUnscheduleFromRunLoop(session(), CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    CFRelease(disk);
    if (!ok) snprintf(err, errlen, "%s", wait.err[0] ? wait.err
                                                     : "the Mac did not release the volume in time");
    return ok;
}

static void return_to_host(NSDictionary *desc) {
    DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session(),
                                             bsd_of(desc).UTF8String);
    if (!disk) return;
    CMDAWait wait = {0, 0, ""};
    DASessionScheduleWithRunLoop(session(), CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    DADiskMount(disk, NULL, kDADiskMountOptionDefault, da_done, &wait);
    pump_until(&wait, 8.0);
    DASessionUnscheduleFromRunLoop(session(), CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    CFRelease(disk);
}

/* Read-only is enforced here, on the node: AROS opens it itself, so its own
 * permissions are what actually hold. */
static void set_node_access(NSDictionary *desc, int writable) {
    NSString *node = [@"/dev/" stringByAppendingString:bsd_of(desc)];
    chmod(node.UTF8String, writable ? 0640 : 0440);
}

/* ---- the mount description -------------------------------------------------- */
static NSString *dos_name(NSDictionary *desc, NSSet *taken) {
    NSMutableString *base = [NSMutableString string];
    NSString *label = desc[(__bridge NSString *)kDADiskDescriptionVolumeNameKey] ?: @"";
    for (NSUInteger i = 0; i < label.length && base.length < 8; i++) {
        unichar c = [label characterAtIndex:i];
        if (isalnum((int)c)) [base appendFormat:@"%C", (unichar)toupper((int)c)];
    }
    if (base.length == 0 || isdigit([base characterAtIndex:0]))
        [base insertString:@"USB" atIndex:0];
    NSString *name = [base substringToIndex:MIN((NSUInteger)8, base.length)];
    NSString *candidate = name;
    for (int n = 1; [taken containsObject:candidate]; n++)
        candidate = [NSString stringWithFormat:@"%@%d",
                     [name substringToIndex:MIN((NSUInteger)7, name.length)], n];
    return candidate;
}

static int write_description(NSDictionary *desc, NSString *name) {
    NSArray *h = handler_for(fs_of(desc));
    if (!h) return 0;
    unsigned long long total = [desc[(__bridge NSString *)kDADiskDescriptionMediaSizeKey]
                                   unsignedLongLongValue];
    unsigned block = [desc[(__bridge NSString *)kDADiskDescriptionMediaBlockSizeKey]
                         unsignedIntValue] ?: 512;
    unsigned long long sectors = total / block;
    if (sectors < 2) return 0;

    NSString *dir = share_path();
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                             withIntermediateDirectories:YES attributes:nil error:NULL];
    NSString *text = [NSString stringWithFormat:
        @"FileSystem      = %@\n"
         "Device          = hostdisk.device\n"
         "Unit            = /dev/%@\n"
         "Flags           = 0\n"
         "Surfaces        = 1\n"
         "BlocksPerTrack  = 1\n"
         "LowCyl          = 0\n"
         "HighCyl         = %llu\n"
         "Reserved        = 0\n"
         "BlockSize       = %u\n"
         "Buffers         = 64\n"
         "BufMemType      = 1\n"
         "StackSize       = 32768\n"
         "Priority        = 5\n"
         "GlobVec         = -1\n"
         "DosType         = %@\n"
         "Activate        = 0\n",
        h[0], bsd_of(desc), sectors - 1, block, h[1]];
    return [text writeToFile:[dir stringByAppendingPathComponent:name]
                  atomically:YES encoding:NSUTF8StringEncoding error:NULL];
}

/* Ask MediaWatch to give a device back, and wait for it to say it has. */
static void request_removal(NSString *name) {
    NSString *dir = share_path();
    NSString *request = [dir stringByAppendingPathComponent:
                            [@".remove-" stringByAppendingString:name]];
    [[NSFileManager defaultManager] removeItemAtPath:
        [dir stringByAppendingPathComponent:name] error:NULL];
    [@"" writeToFile:request atomically:YES encoding:NSUTF8StringEncoding error:NULL];
    for (int i = 0; i < 60; i++) {                       /* MediaWatch polls ~0.8s */
        if (![[NSFileManager defaultManager] fileExistsAtPath:request]) return;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
    }
    /* No MediaWatch running (or it is wedged): the description is gone either
     * way, so the grant will not come back at the next boot. */
    [[NSFileManager defaultManager] removeItemAtPath:request error:NULL];
}

/* ---- the public surface ------------------------------------------------------ */
int cm_media_scan(CMMediaItem *items, int max) {
    if (!items || max <= 0) return 0;
    int n = 0;
    @autoreleasepool {
        NSArray<CMGrant *> *grants = read_grants([NSMutableArray array]);
        for (NSDictionary *desc in attached_media()) {
            if (n >= max) break;
            NSString *ident = identity_of(desc);
            CMMediaItem *it = &items[n++];
            memset(it, 0, sizeof *it);
            snprintf(it->bsd, sizeof it->bsd, "%s", bsd_of(desc).UTF8String ?: "");
            snprintf(it->label, sizeof it->label, "%s",
                     [desc[(__bridge NSString *)kDADiskDescriptionVolumeNameKey] UTF8String] ?: "-");
            snprintf(it->fs, sizeof it->fs, "%s", fs_of(desc).UTF8String ?: "");
            it->size = [desc[(__bridge NSString *)kDADiskDescriptionMediaSizeKey] unsignedLongLongValue];
            it->mounted = mount_point(desc) != nil;
            it->writable = [desc[(__bridge NSString *)kDADiskDescriptionMediaWritableKey] boolValue];
            it->supported = handler_for(fs_of(desc)) != nil;
            for (CMGrant *g in grants) {
                if (![g.ident isEqualToString:ident]) continue;
                it->grant = [g.mode isEqualToString:@"rw"] ? CM_MEDIA_READWRITE : CM_MEDIA_READONLY;
                snprintf(it->aros, sizeof it->aros, "%s", g.name.UTF8String ?: "");
                break;
            }
        }
    }
    return n;
}

int cm_media_set_grant(const char *bsd, int grant, char *err, int errlen) {
    if (err && errlen) err[0] = '\0';
    if (!bsd || !*bsd) return 1;
    @autoreleasepool {
        NSDictionary *found = nil;
        for (NSDictionary *desc in attached_media())
            if ([bsd_of(desc) isEqualToString:@(bsd)]) { found = desc; break; }
        if (!found) { if (err) snprintf(err, errlen, "%s is no longer attached", bsd); return 1; }

        NSMutableArray *others = [NSMutableArray array];
        NSMutableArray *grants = [read_grants(others) mutableCopy];
        NSString *ident = identity_of(found);
        CMGrant *existing = nil;
        for (CMGrant *g in grants) if ([g.ident isEqualToString:ident]) { existing = g; break; }

        if (grant == CM_MEDIA_NONE) {
            if (!existing) return 0;
            request_removal(existing.name);
            [grants removeObject:existing];
            write_grants(grants, others);
            set_node_access(found, 1);
            return_to_host(found);
            return 0;
        }

        if (!handler_for(fs_of(found))) {
            if (err) snprintf(err, errlen, "AROS has no handler for %s", fs_of(found).UTF8String);
            return 1;
        }
        if (grant == CM_MEDIA_READWRITE &&
            ![found[(__bridge NSString *)kDADiskDescriptionMediaWritableKey] boolValue]) {
            if (err) snprintf(err, errlen, "%s is write-protected", bsd);
            return 1;
        }
        char detach_err[160] = "";
        if (!detach_from_host(found, detach_err, sizeof detach_err)) {
            if (err) snprintf(err, errlen, "%s", detach_err);
            return 1;
        }

        NSMutableSet *taken = [NSMutableSet set];
        for (CMGrant *g in grants) if (g != existing) [taken addObject:g.name];
        NSString *name = existing.name ?: dos_name(found, taken);
        set_node_access(found, grant == CM_MEDIA_READWRITE);
        if (!write_description(found, name)) {
            if (err) snprintf(err, errlen, "could not write the mount description");
            return 1;
        }
        if (!existing) {
            existing = [CMGrant new];
            existing.name = name;
            existing.ident = ident;
            [grants addObject:existing];
        }
        existing.fs = fs_of(found);
        existing.mode = (grant == CM_MEDIA_READWRITE) ? @"rw" : @"ro";
        write_grants(grants, others);
    }
    return 0;
}

int cm_media_prepare(void) {
    int n = 0;
    @autoreleasepool {
        NSArray<CMGrant *> *grants = read_grants([NSMutableArray array]);
        if (!grants.count) return 0;
        NSArray<NSDictionary *> *attached = attached_media();
        for (CMGrant *g in grants) {
            for (NSDictionary *desc in attached) {
                if (![identity_of(desc) isEqualToString:g.ident]) continue;
                char err[160] = "";
                if (!detach_from_host(desc, err, sizeof err)) break;
                set_node_access(desc, [g.mode isEqualToString:@"rw"]);
                if (write_description(desc, g.name)) n++;
                break;
            }
        }
    }
    return n;
}

/* ---- hotplug ---------------------------------------------------------------- */
static void (*gObserver)(void *) = NULL;
static void  *gObserverCtx = NULL;

static void media_changed(DADiskRef disk, void *ctx) {
    (void)disk; (void)ctx;
    if (gObserver) gObserver(gObserverCtx);
}

void cm_media_watch(void (*cb)(void *), void *ctx) {
    static int scheduled = 0;
    gObserver = cb;
    gObserverCtx = ctx;
    if (!cb || scheduled) return;
    DARegisterDiskAppearedCallback(watch_session(), NULL, media_changed, NULL);
    DARegisterDiskDisappearedCallback(watch_session(), NULL, media_changed, NULL);
    DASessionScheduleWithRunLoop(watch_session(), CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    scheduled = 1;
}
