// resource.h - resource IDs for DesktopSplitterSetup.
#pragma once

// Driver package, embedded as RCDATA from payload\ (see stage-payload.ps1).
#define IDR_DRIVER_INF          101
#define IDR_DRIVER_DLL          102
#define IDR_DRIVER_CAT          103

// Product files installed into the target directory.
#define IDR_APP_EXE             110   // DesktopSplitter.exe (self-contained single file)
#define IDR_DSCOMP_EXE          111   // dscomp.exe
#define IDR_DEVCREATE_EXE       112   // devcreate.exe
#define IDR_COMPOSITOR_README   113   // compositor-README.md
#define IDR_EDIDOVERRIDE_EXE    114   // edidoverride.exe (EDID override + guarded transaction)

// ---- wizard control IDs ---------------------------------------------------
#define IDC_BTN_BACK               1001
#define IDC_BTN_NEXT               1002
#define IDC_BTN_CANCEL             1003

#define IDC_EDIT_INSTALLDIR        1010
#define IDC_BTN_BROWSE             1011
#define IDC_CHK_DESKTOP_SHORTCUT   1012
#define IDC_CHK_STARTMENU_SHORTCUT 1013
#define IDC_CHK_HIDE_PHYSICAL      1014
#define IDC_STATIC_HIDE_NOTE       1015

#define IDC_LIST_LOG               1020
#define IDC_PROGRESS               1021
#define IDC_STATIC_STEP            1022

#define IDC_CHK_LAUNCH             1030
#define IDC_STATIC_HEADER          1031
#define IDC_STATIC_SUBHEADER       1032
#define IDC_STATIC_BODY            1033

#define IDC_CHK_REMOVE_CERT        1040
