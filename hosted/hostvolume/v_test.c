/* v_test.c — standalone proof of the host-volume Mac glue ([V]).
 *
 * Implemented clean-room from docs/features/host-volume/spec.md
 * (§Unattended verification: [VN] normalization, [VM] sidecar; R-NORM,
 * R-SIDECAR, R-CHARSET). No GPL emulator source (UAE family or vAmiga) was
 * read, searched, or consulted. Follows this project's [OURS] hosted-spike
 * verdict discipline (hosted/cocoametal/d1_test.m: "a file existing is not a
 * PASS — the asserted values are") and the two-sided host/AROS check shape.
 *
 * Since the AROS emul-handler needs the AROS build, this drives the NEW Mac
 * glue directly and exercises it against the REAL macOS filesystem in a temp
 * dir, asserting the observable behaviour the handler's per-host overlay will
 * rely on. It mirrors the AROS-side markers but runs purely host-side.
 *
 *   [V-1] R-NORM   normalize(NFC bytes) == normalize(NFD bytes); then open()/
 *                  readdir() against the real FS and DOCUMENT what APFS itself
 *                  did, asserting our normalization (not the FS) is the thing
 *                  that makes the cross-form lookup deterministic.
 *   [V-2] sidecar  write prot + comment, read back, assert equal; assert the
 *                  sidecar is created ONLY when non-default; assert temp+rename
 *                  atomic (no stray temp left, target replaced in place).
 *   [V-3] charset  a Latin-1 filename containing 'ü' <-> UTF-8 on disk
 *                  round-trips and is found by its UTF-8 name.
 *
 * Prints "[V] PASS ..."/"[V] FAIL ..." with asserted values and exits cleanly,
 * removing the temp dir.
 */
#include "hostvolume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>      /* fork/wait for the concurrency sub-check */

/* ---- tiny assert/report helpers ----------------------------------------- */

static int g_fail = 0;
#define CHECK(cond, fmt, ...) do {                                        \
        int _ok = (cond);                                                 \
        if (!_ok) g_fail = 1;                                             \
        printf("[V]   %-7s " fmt "\n", _ok ? "ok" : "FAIL", ##__VA_ARGS__);\
    } while (0)

/* The same accented string "café" written two ways:
 *   NFC: c a f  U+00E9(é)                -> 63 61 66 C3A9
 *   NFD: c a f  e U+0301(combining acute) -> 63 61 66 65 CC81
 * Byte-different on disk (APFS is a bag of bytes); the SAME logical name. */
static const char NAME_NFC[] = "caf\xC3\xA9";              /* café (composed)   */
static const char NAME_NFD[] = "caf\x65\xCC\x81";         /* café (decomposed) */

/* A Latin-1 filename containing 'ü' (0xFC), as AROS would hold it. */
static const char LAT1_UE[] = "gr\xFC\x6E.txt";           /* "grün.txt" Latin-1 */

/* ---- temp dir lifecycle -------------------------------------------------- */

static char g_dir[4096];

static int make_tmpdir(void) {
    const char *base = getenv("TMPDIR");
    char tmpl[4096];
    snprintf(tmpl, sizeof tmpl, "%saros-hostvol-XXXXXX",
             base ? base : "/tmp/");
    /* TMPDIR usually has a trailing slash on macOS; if not, fix it. */
    if (base && base[0] && base[strlen(base) - 1] != '/')
        snprintf(tmpl, sizeof tmpl, "%s/aros-hostvol-XXXXXX", base);
    char *p = mkdtemp(tmpl);
    if (!p) return -1;
    strncpy(g_dir, p, sizeof g_dir - 1);
    g_dir[sizeof g_dir - 1] = '\0';
    return 0;
}

static void joinp(char *dst, size_t cap, const char *name) {
    snprintf(dst, cap, "%s/%s", g_dir, name);
}

/* Recursively remove the temp dir (files + sidecars + the dir itself). */
static void rm_tmpdir(void) {
    DIR *d = opendir(g_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char p[4096];
            joinp(p, sizeof p, de->d_name);
            unlink(p);
        }
        closedir(d);
    }
    rmdir(g_dir);
}

static int write_file(const char *path, const char *bytes, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    ssize_t w = write(fd, bytes, n);
    close(fd);
    return (w == (ssize_t)n) ? 0 : -1;
}

static int file_exists(const char *path) {
    struct stat st; return lstat(path, &st) == 0;
}

/* ---- [V-1] R-NORM -------------------------------------------------------- */

static void test_v1(void) {
    printf("[V] [V-1] R-NORM: NFC/NFD self-normalization is deterministic\n");

    /* (a) Pure: our normalization collapses the two byte forms to one key. */
    char nfc[256], nfd[256];
    hv_to_nfc(NAME_NFC, nfc, sizeof nfc);
    hv_to_nfc(NAME_NFD, nfd, sizeof nfd);
    CHECK(strcmp(nfc, nfd) == 0,
          "normalize(NFC)==normalize(NFD): \"%s\" == \"%s\" (both NFC)",
          nfc, nfd);
    /* The canonical key must itself be NFC (composed é = C3 A9), 5 bytes. */
    CHECK(strcmp(nfc, NAME_NFC) == 0 && strlen(nfc) == 5,
          "canonical form is NFC (\"%s\", %zu bytes; é=C3A9 composed)",
          nfc, strlen(nfc));
    CHECK(hv_nfc_equal(NAME_NFC, NAME_NFD),
          "hv_nfc_equal(NFC,NFD) == 1");

    /* (b) Against the REAL filesystem. Write the file under its NFD bytes;
     *     then try to open it by its NFC bytes. DOCUMENT what APFS does. */
    char pnfd[4096], pnfc[4096];
    joinp(pnfd, sizeof pnfd, NAME_NFD);
    joinp(pnfc, sizeof pnfc, NAME_NFC);
    write_file(pnfd, "x", 1);

    /* Does the FS fold the two forms? (APFS default = normalization-INSENSITIVE) */
    int fs_folds = (open(pnfc, O_RDONLY) >= 0);
    {
        int fd = open(pnfc, O_RDONLY);
        if (fd >= 0) close(fd);
    }
    printf("[V]   note    FS lookup of NFC-bytes for an NFD-written file: %s\n",
           fs_folds ? "FOUND (filesystem folded the forms)"
                    : "NOT found (filesystem is a strict bag-of-bytes)");

    /* What readdir actually returned (the bytes the FS stored). */
    char stored[256] = "";
    DIR *d = opendir(g_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (hv_is_sidecar_name(de->d_name)) continue;
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            /* the only data file in the dir at this point is our café file */
            if (!strncmp(de->d_name, "caf", 3)) {
                strncpy(stored, de->d_name, sizeof stored - 1);
                break;
            }
        }
        closedir(d);
    }
    /* Print the stored bytes in hex so the FS behaviour is on the record. */
    {
        char hex[600]; size_t h = 0;
        for (size_t i = 0; stored[i] && h + 3 < sizeof hex; i++)
            h += (size_t)snprintf(hex + h, sizeof hex - h, "%02X ",
                                  (unsigned char)stored[i]);
        printf("[V]   note    readdir returned d_name bytes: %s(\"%s\")\n",
               hex, stored);
    }

    /* The load-bearing assertion: WHATEVER the FS did to the bytes, normalizing
     * readdir's result to NFC yields our deterministic key — so a lookup keyed
     * on the normalized form matches regardless of FS folding behaviour. */
    char stored_nfc[256];
    hv_to_nfc(stored, stored_nfc, sizeof stored_nfc);
    CHECK(stored[0] && strcmp(stored_nfc, nfc) == 0,
          "normalize(readdir d_name) == our NFC key \"%s\" (FS-behaviour-independent)",
          nfc);

    unlink(pnfd);
    /* clean up either spelling in case the FS created a second entry */
    unlink(pnfc);
}

/* ---- [V-2] sidecar round-trip -------------------------------------------- */

static void test_v2(void) {
    printf("[V] [V-2] sidecar: .amimeta round-trip + omit-when-default + atomic\n");

    char data[4096], side[4096];
    joinp(data, sizeof data, "doc.txt");
    write_file(data, "hello", 5);

    hv_sidecar_path(data, side, sizeof side);
    /* sidecar name must be ".doc.txt.amimeta" beside the data file */
    {
        const char *sb = strrchr(side, '/'); sb = sb ? sb + 1 : side;
        CHECK(strcmp(sb, ".doc.txt.amimeta") == 0,
              "sidecar path basename == \".doc.txt.amimeta\" (got \"%s\")", sb);
        CHECK(hv_is_sidecar_name(sb) == 1, "hv_is_sidecar_name(\"%s\")==1", sb);
        CHECK(hv_is_sidecar_name("doc.txt") == 0,
              "hv_is_sidecar_name(\"doc.txt\")==0 (data files not skipped)");
    }

    /* (a) Default metadata writes NO sidecar. */
    HVMeta def; memset(&def, 0, sizeof def);
    CHECK(hv_meta_is_default(&def) == 1, "fresh metadata is default");
    CHECK(hv_meta_write(data, &def) == 0, "hv_meta_write(default) ok");
    CHECK(!file_exists(side), "no sidecar created for default metadata (dir stays clean)");

    /* (b) Non-default metadata: a comment + AmigaOS-only Pure|Script bits. */
    HVMeta m; memset(&m, 0, sizeof m);
    m.prot = HV_FIBF_PURE | HV_FIBF_SCRIPT;          /* upper, non-rwx bits */
    strcpy(m.comment, "needs RAD: 50% & a \"quote\"\n");
    CHECK(hv_meta_is_default(&m) == 0, "metadata with comment+bits is non-default");
    CHECK(hv_meta_write(data, &m) == 0, "hv_meta_write(non-default) ok");
    CHECK(file_exists(side), "sidecar created for non-default metadata");

    /* Assert atomicity left no temp file behind. */
    {
        int stray = 0;
        DIR *d = opendir(g_dir);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)))
                if (strstr(de->d_name, ".amimeta.tmp")) stray = 1;
            closedir(d);
        }
        CHECK(!stray, "atomic write left no .tmp file (temp+rename, target replaced)");
    }

    /* (c) Read it back; assert equal. */
    HVMeta r; memset(&r, 0, sizeof r);
    int got = hv_meta_read(data, &r);
    CHECK(got == 1, "hv_meta_read found the sidecar (rc=%d)", got);
    CHECK(r.prot == m.prot, "prot round-trips: 0x%X == 0x%X", r.prot, m.prot);
    CHECK((r.prot & HV_FIBF_AMIGA_ONLY) == (HV_FIBF_PURE | HV_FIBF_SCRIPT),
          "AmigaOS-only bits preserved (PURE|SCRIPT) = 0x%X",
          r.prot & HV_FIBF_AMIGA_ONLY);
    CHECK(strcmp(r.comment, m.comment) == 0,
          "comment round-trips byte-exact (incl. %% , \" and newline)");

    /* (d) Re-defaulting removes the sidecar (keeps dir clean). */
    CHECK(hv_meta_write(data, &def) == 0, "hv_meta_write(default) over existing ok");
    CHECK(!file_exists(side), "sidecar removed when metadata reverts to default");

    /* enumeration must skip the sidecar even while it exists */
    hv_meta_write(data, &m);
    {
        int saw_data = 0, saw_side = 0;
        DIR *d = opendir(g_dir);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {
                if (!strcmp(de->d_name, "doc.txt")) saw_data = 1;
                if (hv_is_sidecar_name(de->d_name)) saw_side = 1; /* would-be-skipped */
            }
            closedir(d);
        }
        CHECK(saw_data && saw_side,
              "sidecar is present-but-skippable: data listed, .amimeta flagged for skip");
    }

    /* (e) Concurrency safety (FINDING 2): many writers race to write the SAME
     * sidecar at once. With a pid-only O_TRUNC temp name they would collide and
     * truncate each other's temp; with mkstemp each gets a unique O_EXCL temp,
     * so every write either lands a complete record or fails cleanly, and the
     * dir is left with NO stray temp file. We fork N children that each write a
     * non-default record, then assert: all succeeded, the surviving sidecar
     * parses to a complete valid record, and no ".amimeta.tmp*" temp lingers. */
    {
        unlink(side);
        const int N = 8;
        HVMeta cm; memset(&cm, 0, sizeof cm);
        cm.prot = HV_FIBF_PURE | HV_FIBF_SCRIPT;
        strcpy(cm.comment, "concurrent writer");
        int rc_all_ok = 1;
        for (int i = 0; i < N; i++) {
            pid_t pid = fork();
            if (pid == 0) {                       /* child */
                int rc = hv_meta_write(data, &cm);
                _exit(rc == 0 ? 0 : 1);
            }
        }
        for (int i = 0; i < N; i++) {
            int st = 0; wait(&st);
            if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) rc_all_ok = 0;
        }
        CHECK(rc_all_ok, "%d concurrent hv_meta_write() to one sidecar all succeeded", N);

        HVMeta cr; memset(&cr, 0, sizeof cr);
        int cgot = hv_meta_read(data, &cr);
        CHECK(cgot == 1 && cr.prot == cm.prot && strcmp(cr.comment, cm.comment) == 0,
              "surviving sidecar after the race is one complete valid record "
              "(rc=%d prot=0x%X)", cgot, cr.prot);

        int stray = 0;
        DIR *d = opendir(g_dir);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)))
                if (strstr(de->d_name, ".amimeta.tmp")) stray = 1;
            closedir(d);
        }
        CHECK(!stray, "no stray .amimeta.tmp temp left after the concurrent race");
    }

    unlink(side);
    unlink(data);
}

/* ---- [V-3] charset round-trip -------------------------------------------- */

static void test_v3(void) {
    printf("[V] [V-3] charset: Latin-1 'ü' name <-> UTF-8 on disk round-trips\n");

    /* AROS Latin-1 -> host UTF-8 */
    char utf8[256];
    hv_latin1_to_utf8(LAT1_UE, utf8, sizeof utf8);
    /* "grün.txt": g r U+00FC(C3 BC) n . t x t */
    CHECK(strcmp(utf8, "gr\xC3\xBCn.txt") == 0,
          "Latin1->UTF8: 0xFC -> C3 BC (\"%s\")", utf8);

    /* Apply the bridge ordering: Latin1->UTF8 then NFC-normalize before the
     * host call (ü is already NFC, so this is a no-op here but exercises it). */
    char hostname[256];
    hv_to_nfc(utf8, hostname, sizeof hostname);

    /* Create the file under its UTF-8 host name; assert it appears + is found. */
    char path[4096];
    joinp(path, sizeof path, hostname);
    CHECK(write_file(path, "ok", 2) == 0, "created host file by its UTF-8 name");
    CHECK(file_exists(path), "host file exists under UTF-8 name on disk");

    /* readdir it back; UTF-8 -> Latin-1 must recover the original AROS bytes. */
    char found_utf8[256] = "";
    DIR *d = opendir(g_dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (hv_is_sidecar_name(de->d_name)) continue;
            if (strstr(de->d_name, "gr") && strstr(de->d_name, ".txt")) {
                strncpy(found_utf8, de->d_name, sizeof found_utf8 - 1);
                break;
            }
        }
        closedir(d);
    }
    char back_latin1[256];
    /* normalize then map back to Latin-1 (the host->AROS pipeline order) */
    char fnfc[256];
    hv_to_nfc(found_utf8, fnfc, sizeof fnfc);
    hv_utf8_to_latin1(fnfc, back_latin1, sizeof back_latin1);
    CHECK(found_utf8[0] && strcmp(back_latin1, LAT1_UE) == 0,
          "UTF8->Latin1 recovers original AROS bytes \"gr\\xFCn.txt\" (got 0x%02X at [2])",
          (unsigned char)back_latin1[2]);

    /* Escape round-trip for an un-mappable code point (U+0153 'œ'): out then
     * back reproduces the same UTF-8, proving R-ESCAPE reversibility. */
    const char *oe = "f\xC5\x93.txt";        /* "fœ.txt" UTF-8 (œ=U+0153) */
    char esc[256], unesc[256];
    hv_utf8_to_latin1(oe, esc, sizeof esc);  /* -> "f%u0153.txt" (ASCII) */
    CHECK(strcmp(esc, "f%u0153.txt") == 0,
          "un-mappable U+0153 escapes to ASCII \"%s\"", esc);

    /* THE round-trip the bridge must guarantee (FINDING 1): the escaped AROS
     * name fed back through Latin1->UTF8 must DECODE %uXXXX to the real code
     * point, reproducing the host UTF-8 byte-for-byte (NOT the literal ASCII
     * string). fœ.txt -> "f%u0153.txt" -> fœ.txt. */
    hv_latin1_to_utf8(esc, unesc, sizeof unesc);
    CHECK(strcmp(unesc, oe) == 0,
          "AROS escaped name decodes back to byte-identical host UTF-8 \"f\\xC5\\x93.txt\" "
          "(got %02X %02X %02X at [1..3])",
          (unsigned char)unesc[1], (unsigned char)unesc[2], (unsigned char)unesc[3]);

    /* Full UTF-8 -> AROS -> UTF-8 round-trip exercised end to end. */
    char esc2[256], rt[256];
    hv_utf8_to_latin1(oe, esc2, sizeof esc2);
    hv_latin1_to_utf8(esc2, rt, sizeof rt);
    CHECK(strcmp(rt, oe) == 0,
          "host->AROS->host round-trips the un-mappable name byte-identically");

    /* Marker round-trip: the literal '%' must self-escape to "%u0025" on the
     * way to AROS and DECODE back to a literal '%' on the way to the host. */
    char pct[256], pct_back[256];
    hv_utf8_to_latin1("50%done", pct, sizeof pct);      /* % -> %u0025 */
    CHECK(strcmp(pct, "50%u0025done") == 0,
          "marker '%%' self-escapes to \"%s\" (symmetric round-trip)", pct);
    hv_latin1_to_utf8(pct, pct_back, sizeof pct_back);  /* %u0025 -> % */
    CHECK(strcmp(pct_back, "50%done") == 0,
          "marker escape %%u0025 decodes back to a literal '%%' (got \"%s\")", pct_back);

    /* Ambiguity policy: a bare '%' NOT followed by "uXXXX" is a literal '%' and
     * passes through Latin1->UTF8 unchanged (not mistaken for an escape). */
    char bare[256];
    hv_latin1_to_utf8("a%b%z.txt", bare, sizeof bare);
    CHECK(strcmp(bare, "a%b%z.txt") == 0,
          "bare '%%' not followed by uXXXX is a literal (\"%s\")", bare);

    unlink(path);
}

/* ---- driver -------------------------------------------------------------- */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[V] host-volume Mac glue: NFC-norm + .amimeta sidecar + Latin1<->UTF8 (standalone)\n");

    if (make_tmpdir() != 0) {
        printf("[V] FAIL could not create temp dir (errno=%d)\n", errno);
        return 1;
    }
    printf("[V] tmpdir=%s\n", g_dir);

    test_v1();
    test_v2();
    test_v3();

    rm_tmpdir();

    if (g_fail) {
        printf("[V] FAIL see checks above\n");
        return 1;
    }
    printf("[V] PASS R-NORM(NFC==NFD deterministic) + sidecar(prot+comment round-trip, "
           "omit-default, atomic) + charset(Latin1<->UTF8 'ü' round-trip, escape reversible)\n");
    return 0;
}
