/*
 * guids.c - the DirectX interface and device GUID constants, instantiated once.
 *
 * Wine's headers declare these with DEFINE_GUID, which only *declares* unless
 * INITGUID is defined first; on Windows the definitions come from dxguid.lib, and
 * this tree links no Wine libraries, only its headers.
 *
 * It has to be exactly one translation unit for the whole program, which is why this
 * file exists rather than each shim doing it for itself. dinput8.c used to, and when
 * dsound.c did the same the link failed on several dozen *unrelated* symbols -
 * IID_IUnknown, IID_IErrorInfo, IID_IXMLDOMParseError - because INITGUID applies to
 * every GUID in every header the unit transitively includes, and both units reach
 * unknwn.h and oaidl.h. Splitting by DLL is the wrong axis: the axis is "who emits
 * the constants", and the answer has to be one place.
 *
 * This is in the core shim rather than beside one of the SDL-backed libraries so
 * that it is always linked, whichever of them the build includes. The file contains
 * no code and depends on nothing.
 */
#define INITGUID

#include <nfsu2/win32_compat.h>

/* Every header whose GUIDs anything here references. Adding a shim that needs
 * another DirectX GUID means adding its header to this list, not another INITGUID. */
#include <dinput.h>
#include <dsound.h>
