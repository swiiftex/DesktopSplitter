// Guids.cpp - the single translation unit that gives storage to
// GUID_DEVINTERFACE_DESKSPLIT (used to talk to the driver for "revert split").
//
// <initguid.h> must come AFTER every system header: several SDK headers declare
// the same GUID/DEVPROPKEY in more than one place, which is harmless as
// repeated extern declarations but produces C2374 "redefinition; multiple
// initialization" once INITGUID is in effect.
#include <windows.h>
#include <winioctl.h>

#include <initguid.h>
#include "DeskSplitProtocol.h"
