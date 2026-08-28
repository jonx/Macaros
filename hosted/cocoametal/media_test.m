/* media_test.m — [MEDIA] the host media broker, driven the way the Settings
 * window drives it.
 *
 * Attaches a disposable exFAT image as a real /dev/disk node (the same kind of
 * node a USB stick gets), then: sees it in the list, grants it read-only, grants
 * it read/write, and withdraws it. After each step it checks what the guest
 * would actually act on — the mount description in the shared directory, the
 * grant recorded in aros-host.conf, and the device node's own permissions.
 *
 * Runs unattended: no hardware, no window server, no privileges.
 */
#import <Foundation/Foundation.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

#include "cocoametal_media.h"

static int failures = 0;

static void ck(int ok, const char *what) {
    printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) failures++;
}

static NSString *run(NSString *tool, NSArray *args) {
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:tool];
    task.arguments = args;
    NSPipe *pipe = [NSPipe pipe];
    task.standardOutput = pipe;
    task.standardError = [NSPipe pipe];
    NSError *err = nil;
    if (![task launchAndReturnError:&err]) return nil;
    NSData *out = [pipe.fileHandleForReading readDataToEndOfFile];
    [task waitUntilExit];
    return [[[NSString alloc] initWithData:out encoding:NSUTF8StringEncoding]
               stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

/* A raw exFAT image attached as /dev/diskN, user-owned, exactly like a stick the
 * user has just plugged in: macOS mounts it in the Finder first. */
static NSString *attach_fixture(NSString *image) {
    NSString *out = run(@"/usr/bin/hdiutil",
                        @[@"attach", @"-imagekey",
                          @"diskimage-class=CRawDiskImage", image]);
    for (NSString *word in [(out ?: @"") componentsSeparatedByString:@" "])
        if ([word hasPrefix:@"/dev/disk"]) return word;
    return nil;
}

static int find_item(const char *bsd, CMMediaItem *out) {
    CMMediaItem items[32];
    int n = cm_media_scan(items, 32);
    for (int i = 0; i < n; i++)
        if (strcmp(items[i].bsd, bsd) == 0) { *out = items[i]; return 1; }
    return 0;
}

static NSString *description_for(CMMediaItem *it) {
    NSString *path = [@(cm_media_dir()) stringByAppendingPathComponent:@(it->aros)];
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:NULL];
}

static mode_t node_mode(const char *bsd) {
    struct stat st;
    char path[64];
    snprintf(path, sizeof path, "/dev/%s", bsd);
    if (stat(path, &st) != 0) return 0;
    return st.st_mode & 0777;
}

int main(void) {
    @autoreleasepool {
        NSString *work = [NSTemporaryDirectory() stringByAppendingPathComponent:@"cm-media-test"];
        NSString *image = [work stringByAppendingPathComponent:@"stick.img"];
        NSString *share = [work stringByAppendingPathComponent:@"share"];
        NSString *conf  = [work stringByAppendingPathComponent:@"aros-host.conf"];
        [[NSFileManager defaultManager] removeItemAtPath:work error:NULL];
        [[NSFileManager defaultManager] createDirectoryAtPath:work
                                 withIntermediateDirectories:YES attributes:nil error:NULL];
        setenv("MACAROS_MEDIA_DIR", [share stringByAppendingPathComponent:@".macaros-media"].UTF8String, 1);
        setenv("AROS_HOST_CONF", conf.UTF8String, 1);
        [@"memory 1280\n" writeToFile:conf atomically:YES encoding:NSUTF8StringEncoding error:NULL];

        /* 32 MiB is the smallest volume newfs_exfat will make with room to spare. */
        run(@"/usr/bin/hdiutil", @[@"create", @"-size", @"32m", @"-fs", @"exfat",
                                   @"-volname", @"MEDIATEST", @"-layout", @"NONE",
                                   @"-type", @"UDIF", @"-ov", image]);
        NSString *imageFile = [image stringByAppendingPathExtension:@"dmg"];
        if (![[NSFileManager defaultManager] fileExistsAtPath:imageFile]) imageFile = image;
        NSString *node = attach_fixture(imageFile);
        if (!node) { printf("[MEDIA] FAIL: could not attach the test image\n"); return 1; }
        const char *bsd = [node substringFromIndex:5].UTF8String;
        printf("[MEDIA] fixture attached as %s\n", node.UTF8String);

        CMMediaItem it;
        char err[192] = "";
        int ok;

        ck(find_item(bsd, &it), "the attached medium is listed");
        ck(strcmp(it.fs, "exfat") == 0, "listed as exFAT");
        ck(it.supported == 1, "AROS has a handler for it");
        ck(it.grant == CM_MEDIA_NONE, "nothing is granted to start with");
        ck(it.mounted == 1, "macOS has it mounted, as it would a freshly plugged stick");

        ok = cm_media_set_grant(bsd, CM_MEDIA_READONLY, err, sizeof err) == 0;
        ck(ok, err[0] ? err : "granted read-only");
        ck(find_item(bsd, &it) && it.grant == CM_MEDIA_READONLY, "the grant is read-only");
        ck(it.mounted == 0, "macOS gave the volume up, so only one system writes it");
        NSString *text = description_for(&it);
        ck([text containsString:@"Device          = hostdisk.device"], "AROS is told to use hostdisk.device");
        ck([text containsString:[NSString stringWithFormat:@"Unit            = %@", node]],
           "the mount description names the current device node");
        ck([text containsString:@"FileSystem      = exfat-handler"], "the exFAT handler is selected");
        ck(node_mode(bsd) == 0440, "the device node is read-only to the guest");

        ok = cm_media_set_grant(bsd, CM_MEDIA_READWRITE, err, sizeof err) == 0;
        ck(ok, err[0] ? err : "granted read/write");
        ck(find_item(bsd, &it) && it.grant == CM_MEDIA_READWRITE, "the grant is read/write");
        ck(node_mode(bsd) == 0640, "the device node is writable to the guest");

        NSString *confText = [NSString stringWithContentsOfFile:conf encoding:NSUTF8StringEncoding error:NULL];
        ck([confText containsString:@"memory 1280"], "the other settings survive a grant");
        ck([confText containsString:@"media "], "the grant is recorded");

        ck(cm_media_prepare() == 1, "a recorded grant is re-resolved at boot");

        ok = cm_media_set_grant(bsd, CM_MEDIA_NONE, err, sizeof err) == 0;
        ck(ok, err[0] ? err : "withdrawn");
        ck(find_item(bsd, &it) && it.grant == CM_MEDIA_NONE, "nothing is granted any more");
        ck(![[NSFileManager defaultManager] fileExistsAtPath:
                [@(cm_media_dir()) stringByAppendingPathComponent:@"MEDIATEST"]],
           "the mount description is gone");
        ck(node_mode(bsd) == 0640, "the device node is handed back writable");
        ck(find_item(bsd, &it) && it.mounted == 1, "macOS has the volume back");

        run(@"/usr/bin/hdiutil", @[@"detach", node, @"-quiet"]);
        [[NSFileManager defaultManager] removeItemAtPath:work error:NULL];
    }
    printf("[MEDIA] %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
