/* Target-side T2/T8/T13b probe for the sparse large exFAT fixture. */
#include <stdio.h>

#include <dos/dos.h>
#include <dos/dos64.h>
#include <proto/dos.h>
#include <proto/dos64.h>
#include <proto/exec.h>

struct Library *DOS64Base;

static int fail(const char *name, const char *what, long long got)
{
    printf("[EXFATSPARSE] FAIL %s %s (%lld, IoErr %ld)\n",
           name, what, got, (long)IoErr());
    return 20;
}

static int read_at(BPTR file, const char *name, QUAD position, UBYTE expected)
{
    UBYTE got = 0xff;

    if (Seek64(file, OFFSET_BEGINNING, position) < 0)
        return fail(name, "Seek64", (long long)position);
    if (GetFilePosition64(file) != position)
        return fail(name, "GetFilePosition64", (long long)GetFilePosition64(file));
    if (Read64(file, &got, 1) != 1)
        return fail(name, "Read64", got);
    if (got != expected)
        return fail(name, "sentinel", got);
    return 0;
}

static int check_file(const char *path, QUAD size, QUAD last,
                      UBYTE first_marker, UBYTE last_marker)
{
    struct FileInfoBlock64 fib;
    BPTR file = Open(path, MODE_OLDFILE);
    int result = 0;

    if (file == BNULL)
        return fail(path, "Open", -1);
    if (!IsFileSystem64(file))
        result = fail(path, "IsFileSystem64", 0);
    else if (GetFileSize64(file) != size)
        result = fail(path, "GetFileSize64", (long long)GetFileSize64(file));
    else if (!ExamineFH64(file, &fib, NULL))
        result = fail(path, "ExamineFH64", -1);
    else if (fib.fib_Size != (UQUAD)size)
        result = fail(path, "ExamineFH64 size", (long long)fib.fib_Size);
    else if ((result = read_at(file, path, 0, first_marker)) == 0)
        result = read_at(file, path, last, last_marker);
    Close(file);
    return result;
}

static int check_plus_one(void)
{
    const char *path = "EXFAT9:Plus1.bin";
    BPTR file;
    int result;

    result = check_file(path, 0x100000001LL, 0x100000000LL, 'P', 'p');
    if (result != 0)
        return result;
    file = Open(path, MODE_OLDFILE);
    if (file == BNULL)
        return fail(path, "reopen", -1);
    result = read_at(file, path, 0xffffffffLL, 'B');
    Close(file);
    return result;
}

static int check_zero_tail(void)
{
    const char *path = "EXFAT9:ZeroTail.bin";
    const QUAD valid_length = 0x100003LL;
    UBYTE pair[2] = {0xff, 0xff};
    BPTR file = Open(path, MODE_OLDFILE);
    int result = 0;

    if (file == BNULL)
        return fail(path, "Open", -1);
    if (GetFileSize64(file) != 0x200000LL)
        result = fail(path, "GetFileSize64", (long long)GetFileSize64(file));
    else if ((result = read_at(file, path, 0, 'Z')) != 0)
        ;
    else if (Seek64(file, OFFSET_BEGINNING, valid_length - 1) < 0)
        result = fail(path, "Seek64 crossing VDL", -1);
    else if (Read64(file, pair, 2) != 2 || pair[0] != 'V' || pair[1] != 0)
        result = fail(path, "ValidDataLength crossing", pair[1]);
    else if ((result = read_at(file, path, valid_length, 0)) != 0)
        ;
    else
        result = read_at(file, path, 0x1fffffLL, 0);
    Close(file);
    return result;
}

int main(void)
{
    int result;

    DOS64Base = OpenLibrary("dos64.library", 50);
    if (DOS64Base == NULL)
        return fail("dos64.library", "OpenLibrary", -1);

    result = check_file("EXFAT9:Minus1.bin", 0xffffffffLL,
                        0xfffffffeLL, 'M', 'm');
    if (result == 0)
        result = check_file("EXFAT9:Exact4G.bin", 0x100000000LL,
                            0xffffffffLL, 'E', 'e');
    if (result == 0)
        result = check_plus_one();
    if (result == 0)
        result = check_zero_tail();

    CloseLibrary(DOS64Base);
    if (result == 0)
        printf("[EXFATSPARSE] PASS T2/T8/T13b sizes, sentinels and zero tail\n");
    return result;
}
