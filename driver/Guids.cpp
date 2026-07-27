// Guids.cpp - the single translation unit that gives storage to the GUIDs
// declared in shared/DeskSplitProtocol.h (GUID_DEVINTERFACE_DESKSPLIT).
//
// <initguid.h> redefines DEFINE_GUID so that every subsequent DEFINE_GUID emits
// an initialised object instead of an extern declaration. It must therefore be
// included AFTER all system headers - several SDK headers declare the same GUID
// or DEVPROPKEY in more than one place (winioctl.h vs devpkey.h/ntddstor.h),
// which is harmless as repeated extern declarations but produces C2374
// "redefinition; multiple initialization" once INITGUID is in effect.
//
// Keeping INITGUID confined to this file means no other translation unit, and
// no shared header, ever sees it.

#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

#include <initguid.h>
#include "DeskSplitProtocol.h"
