// resource.h -- shared control IDs between mgmp_loader.rc (the DIALOGEX
// definition) and mgmp_loader_dialog.cpp (the code that drives it). Kept as
// its own tiny header, in res/ alongside the .rc that uses it, rather than
// folded into either file -- both sides need the SAME numbers and only one
// of them can be the source of truth.
#pragma once

#define IDD_LAUNCHER      101

#define IDC_RADIO_HOST    1001
#define IDC_RADIO_CLIENT  1002
#define IDC_LABEL_ADDR    1003
#define IDC_EDIT_ADDR     1004
#define IDC_LABEL_PORT    1005
#define IDC_EDIT_PORT     1006
#define IDC_CHECK_NOSHOW  1007
