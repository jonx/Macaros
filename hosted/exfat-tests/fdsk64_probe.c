/*
 * FDSK64Probe -- target-side transport gate for fdsk.device.
 *
 * The host fixture puts distinct marker bytes at offset zero and at 4 GiB - 1,
 * 4 GiB, and 4 GiB + 1.  A successful read at 4 GiB therefore proves both
 * halves of the NSD offset reached fdsk; a device that truncates to io_Offset
 * reads the deliberately different byte at zero and fails this probe.
 */

#include <exec/types.h>
#include <devices/newstyle.h>
#include <devices/trackdisk.h>
#include <exec/errors.h>
#include <proto/dos.h>
#include <proto/exec.h>

static int fail(CONST_STRPTR what, SIPTR code)
{
    Printf("[FDSK64] FAIL %s (%ld)\n", what, code);
    return 20;
}

static BOOL supports_command(const struct NSDeviceQueryResult *query,
                             UWORD wanted)
{
    const UWORD *command = query->SupportedCommands;

    if (command == NULL)
        return FALSE;

    while (*command != 0)
    {
        if (*command == wanted)
            return TRUE;
        command++;
    }
    return FALSE;
}

static int read_marker(struct IOExtTD *request, ULONG high, ULONG low,
                       UBYTE expected)
{
    UBYTE got = 0;
    LONG error;

    request->iotd_Req.io_Command = NSCMD_TD_READ64;
    request->iotd_Req.io_Data = &got;
    request->iotd_Req.io_Length = 1;
    request->iotd_Req.io_Offset = low;
    request->iotd_Req.io_Actual = high;

    error = DoIO((struct IORequest *)request);
    if (error != 0 || request->iotd_Req.io_Error != 0)
        return fail("NSCMD_TD_READ64 error", error != 0 ? error : request->iotd_Req.io_Error);
    if (request->iotd_Req.io_Actual != 1)
        return fail("short NSCMD_TD_READ64", (SIPTR)request->iotd_Req.io_Actual);
    if (got != expected)
        return fail("wrong marker (offset truncated or data misplaced)", got);

    return 0;
}

static int write_marker(struct IOExtTD *request, ULONG high, ULONG low,
                        UBYTE marker)
{
    LONG error;

    request->iotd_Req.io_Command = NSCMD_TD_WRITE64;
    request->iotd_Req.io_Data = &marker;
    request->iotd_Req.io_Length = 1;
    request->iotd_Req.io_Offset = low;
    request->iotd_Req.io_Actual = high;

    error = DoIO((struct IORequest *)request);
    if (error != 0 || request->iotd_Req.io_Error != 0)
        return fail("NSCMD_TD_WRITE64 error", error != 0 ? error : request->iotd_Req.io_Error);
    if (request->iotd_Req.io_Actual != 1)
        return fail("short NSCMD_TD_WRITE64", (SIPTR)request->iotd_Req.io_Actual);

    return 0;
}

int main(void)
{
    struct MsgPort *port;
    struct IOExtTD *request;
    struct NSDeviceQueryResult query;
    int result;

    port = CreateMsgPort();
    if (port == NULL)
        return fail("CreateMsgPort", ERROR_NO_FREE_STORE);

    request = (struct IOExtTD *)CreateIORequest(port, sizeof(*request));
    if (request == NULL)
    {
        DeleteMsgPort(port);
        return fail("CreateIORequest", ERROR_NO_FREE_STORE);
    }

    if (OpenDevice("fdsk.device", 3, (struct IORequest *)request, 0) != 0)
    {
        DeleteIORequest((struct IORequest *)request);
        DeleteMsgPort(port);
        return fail("OpenDevice fdsk.device unit 3", IoErr());
    }

    query.DevQueryFormat = 0;
    request->iotd_Req.io_Command = NSCMD_DEVICEQUERY;
    request->iotd_Req.io_Data = &query;
    request->iotd_Req.io_Length = sizeof(query);
    request->iotd_Req.io_Actual = 0;
    if (DoIO((struct IORequest *)request) != 0 ||
        request->iotd_Req.io_Error != 0 ||
        !supports_command(&query, NSCMD_TD_READ64) ||
        !supports_command(&query, NSCMD_TD_WRITE64))
    {
        CloseDevice((struct IORequest *)request);
        DeleteIORequest((struct IORequest *)request);
        DeleteMsgPort(port);
        return fail("NSCMD_TD_READ64 not advertised", request->iotd_Req.io_Error);
    }

    result = read_marker(request, 0, 0, 'L');
    if (result == 0)
        result = read_marker(request, 0, 0xffffffffUL, 'B');
    if (result == 0)
        result = read_marker(request, 1, 0, 'E');
    if (result == 0)
        result = read_marker(request, 1, 1, 'P');
    if (result == 0)
        result = write_marker(request, 1, 2, 'W');
    if (result == 0)
        result = read_marker(request, 1, 2, 'W');

    CloseDevice((struct IORequest *)request);
    DeleteIORequest((struct IORequest *)request);
    DeleteMsgPort(port);

    if (result == 0)
        Printf("[FDSK64] PASS NSD read/write at 4 GiB - 1, 4 GiB, and 4 GiB + 1\n");

    return result;
}
