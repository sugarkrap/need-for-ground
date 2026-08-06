/*
 * tapi.c - the TAPI (telephony) entry points, as honest failures.
 *
 * NFSU2's 2004 multiplayer could dial another PC over a modem; TAPI is how it
 * asked Windows to do that. There is no modem to dial and the replacement
 * multiplayer will be socket-based (see ws2_32/), so none of this is
 * implemented - but the imports must resolve, and they must fail the way a
 * machine with no telephony device fails.
 *
 * lineInitialize returning LINEERR_NODEVICE is the important one: it is the
 * first call the game makes, and a device count of zero is what makes it skip
 * every later line* call and drop the modem option from its UI. The rest are
 * here for the case where some path calls them anyway.
 *
 * Note the return type is DWORD, not LONG, in Wine's tapi.h - the LINEERR_*
 * constants are large positive values (0x8000000x), so a signed return would
 * make a naive `result < 0` check behave differently here than on Windows.
 */
#include <nfsu2/win32_compat.h>
#include <nfsu2/win32_shim.h>

#include <tapi.h>

#include <string.h>

DWORD WINAPI lineInitialize(LPHLINEAPP app, HINSTANCE instance, LINECALLBACK callback,
                            LPCSTR app_name, LPDWORD num_devs)
{
    (void)instance; (void)callback; (void)app_name;

    NFSU2_STUB("lineInitialize (no telephony device)");
    if (app)
        *app = NULL;
    /* Report zero lines as well as failing: some callers look at only one. */
    if (num_devs)
        *num_devs = 0;
    return LINEERR_NODEVICE;
}

DWORD WINAPI lineShutdown(HLINEAPP app)
{
    (void)app;
    /* Succeeding here is harmless and keeps a cleanup path from logging an
     * error for something it never successfully initialised. */
    return 0;
}

DWORD WINAPI lineNegotiateAPIVersion(HLINEAPP app, DWORD device, DWORD low_version,
                                     DWORD high_version, LPDWORD negotiated,
                                     LPLINEEXTENSIONID extension_id)
{
    (void)app; (void)device; (void)low_version; (void)high_version;

    NFSU2_STUB("lineNegotiateAPIVersion (no telephony device)");
    if (negotiated)
        *negotiated = 0;
    if (extension_id)
        memset(extension_id, 0, sizeof(*extension_id));
    return LINEERR_NODEVICE;
}

DWORD WINAPI lineGetDevCapsA(HLINEAPP app, DWORD device, DWORD version, DWORD ext_version,
                             LPLINEDEVCAPS caps)
{
    (void)app; (void)device; (void)version; (void)ext_version; (void)caps;
    NFSU2_STUB("lineGetDevCapsA (no telephony device)");
    return LINEERR_NODEVICE;
}

DWORD WINAPI lineOpen(HLINEAPP app, DWORD device, LPHLINE line, DWORD version,
                      DWORD ext_version, DWORD callback_instance, DWORD privileges,
                      DWORD media_modes, LPLINECALLPARAMS call_params)
{
    (void)app; (void)device; (void)version; (void)ext_version; (void)callback_instance;
    (void)privileges; (void)media_modes; (void)call_params;

    NFSU2_STUB("lineOpen (no telephony device)");
    if (line)
        *line = NULL;
    return LINEERR_NODEVICE;
}

DWORD WINAPI lineClose(HLINE line)
{
    (void)line;
    return 0;
}

DWORD WINAPI lineGetIDA(HLINE line, DWORD address, HCALL call, DWORD select,
                        LPVARSTRING device_id, LPCSTR device_class)
{
    (void)line; (void)address; (void)call; (void)select; (void)device_id; (void)device_class;
    NFSU2_STUB("lineGetIDA (no telephony device)");
    return LINEERR_NODEVICE;
}

DWORD WINAPI lineMakeCallA(HLINE line, LPHCALL call, LPCSTR dest_address,
                           DWORD country_code, LPLINECALLPARAMS call_params)
{
    (void)line; (void)dest_address; (void)country_code; (void)call_params;

    NFSU2_STUB("lineMakeCallA (no telephony device)");
    if (call)
        *call = NULL;
    return LINEERR_NODEVICE;
}

DWORD WINAPI lineAnswer(HCALL call, LPCSTR user_info, DWORD size)
{
    (void)call; (void)user_info; (void)size;
    NFSU2_STUB("lineAnswer (no telephony device)");
    return LINEERR_NODEVICE;
}

/*
 * tapi32.dll exports these *without* an A suffix - `lineGetDevCaps`, not
 * `lineGetDevCapsA` - and that is what the game's import table names. Wine's
 * tapi.h maps the plain name to the A one for source compatibility, so the
 * suffixed definitions above do not produce the symbol the import needs. These
 * aliases do.
 */
#undef lineGetDevCaps
#undef lineGetID
#undef lineMakeCall

DWORD WINAPI lineGetDevCaps(HLINEAPP app, DWORD device, DWORD version, DWORD ext_version,
                            LPLINEDEVCAPS caps)
{
    return lineGetDevCapsA(app, device, version, ext_version, caps);
}

DWORD WINAPI lineGetID(HLINE line, DWORD address, HCALL call, DWORD select,
                       LPVARSTRING device_id, LPCSTR device_class)
{
    return lineGetIDA(line, address, call, select, device_id, device_class);
}

DWORD WINAPI lineMakeCall(HLINE line, LPHCALL call, LPCSTR dest_address, DWORD country_code,
                          LPLINECALLPARAMS call_params)
{
    return lineMakeCallA(line, call, dest_address, country_code, call_params);
}
