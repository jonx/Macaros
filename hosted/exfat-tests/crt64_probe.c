/*
 * CRT64Probe -- native AArch64 proof that the POSIX and stdio CRT paths use
 * dos64 packets on a file past 4 GiB.  The companion fdsk probe establishes
 * that the block transport below it is sound; this one catches a CRT fallback
 * to a signed 32-bit Seek()/FileInfoBlock.
 */

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <proto/dos.h>
#include <proto/dos64.h>
#include <proto/exec.h>

#define FILE_NAME "SYS:DiskImages/Unit3"
#define FILE_SIZE 4294967299LL

static int fail(const char *what, long long got)
{
    printf("[CRT64] FAIL %s (%lld)\n", what, got);
    return 20;
}

int main(void)
{
    struct Library *DOS64Base;
    BPTR dos_file;
    struct FileInfoBlock64 fib;
    FILE *stream;
    int fd;
    int byte;
    struct stat st;
    long position;
    off_t wanted;
    QUAD dos_size;

    DOS64Base = OpenLibrary("dos64.library", 50);
    dos_file = Open(FILE_NAME, MODE_OLDFILE);
    if (DOS64Base == NULL || dos_file == BNULL)
    {
        if (dos_file != BNULL)
            Close(dos_file);
        if (DOS64Base != NULL)
            CloseLibrary(DOS64Base);
        return fail("open dos64 backing file", -1);
    }
    if (!IsFileSystem64(dos_file))
    {
        Close(dos_file);
        CloseLibrary(DOS64Base);
        return fail("IsFileSystem64", 0);
    }
    dos_size = GetFileSize64(dos_file);
    if (!ExamineFH64(dos_file, &fib, NULL))
    {
        Close(dos_file);
        CloseLibrary(DOS64Base);
        return fail("ExamineFH64", -1);
    }
    Close(dos_file);
    CloseLibrary(DOS64Base);
    if (dos_size != (QUAD)FILE_SIZE)
        return fail("GetFileSize64", (long long)dos_size);
    if (fib.fib_Size != (UQUAD)FILE_SIZE)
        return fail("ExamineFH64 size", (long long)fib.fib_Size);

    stream = fopen(FILE_NAME, "rb");
    if (stream == NULL)
        return fail("fopen", -1);

    if (fseek(stream, (long)0x100000000ULL, SEEK_SET) != 0) {
        fclose(stream);
        return fail("fseek 4 GiB", -1);
    }
    position = ftell(stream);
    if (position != (long)0x100000000ULL) {
        fclose(stream);
        return fail("ftell after fseek", position);
    }
    byte = fgetc(stream);
    if (byte != 'E') {
        fclose(stream);
        return fail("fgetc at 4 GiB", byte);
    }
    fclose(stream);

    fd = open(FILE_NAME, O_RDONLY);
    if (fd < 0)
        return fail("open", fd);

    wanted = (off_t)0x100000001ULL;
    if (lseek(fd, wanted, SEEK_SET) != wanted) {
        close(fd);
        return fail("lseek 4 GiB + 1", -1);
    }
    if (read(fd, &byte, 1) != 1 || byte != 'P') {
        close(fd);
        return fail("read at 4 GiB + 1", byte);
    }
    if (fstat(fd, &st) != 0 || st.st_size != (off_t)FILE_SIZE) {
        close(fd);
        return fail("fstat size", (long long)st.st_size);
    }
    close(fd);

    printf("[CRT64] PASS fopen/fseek/ftell/lseek/fstat past 4 GiB\n");
    return 0;
}
