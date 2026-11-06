# xam ordinals FM4 resolves at run time

FM4's GetProcAddress wrapper sub_82759288 calls XexGetProcedureAddress(module, ordinal) and treats a result
equal to the module's DllBase (+0x18) as STATUS_ENTRYPOINT_NOT_FOUND. Its 37 call sites (ppc/, `li r4,N` just
before `bl 0x82759288`; OBSERVED by grep of the translation) pass:

| ordinal | name (Xenia xam_table.inc line) | Xenia |
|---|---|---|
| 2815 (0xAFF) | XamPartyGetUserList (:1686) | |
| 2816 (0xB00) | XamPartySendGameInvites (:1687) | |
| 2827 (0xB0B) | XamPartySetCustomData (:1698) | |
| 2832 (0xB10) | XamPartyGetBandwidth (:1703) | |
| 773 (0x305) | XamShowPartyUI (:569) | |
| 779 (0x30B) | XamShowCommunitySessionsUI (:575) | |
| 2491 (0x9BB) | XamMediaVerificationCreate (:1586) | |
| 2492 (0x9BC) | XamMediaVerificationClose (:1587) | |
| 2493 (0x9BD) | XamMediaVerificationVerify (:1588) | |
| 2494 (0x9BE) | XamMediaVerificationFailedBlocks (:1589) | |
| 765 (0x2FD) | XamReadTileToTextureEx (:561) | |
| 873 (0x369) | XuiAlloc (:669) | |
| 2271 (0x8DF) | XamNuiGetTrueColorInfo (:1568) | |

Xenia: Party and Show*UI entries are kStub (xam_party.cc, xam_ui.cc); the others have no implementation
(GetProcAddressByOrdinal then returns 0). The remaining call sites pass ordinals 1-14 with other module handles
(title modules; not added). Early boot runs requested 0xAFF then 0xB00.

## Static initializers (sub_82D38xxx)

Each calls sub_82C9F060(module name, ordinal) and stores the result; module names "xam.xex" (0x82078830) and
"xboxkrnl.exe" (0x82068AB8) read from the image. 10 call sites (`li r4,N` before `bl 0x82c9f060`):

- xam.xex 442 (0x1BA) XamInputGetUserVibrationLevel (Xenia xam_table.inc:239)
- xam.xex 520 (0x208) XamUserGetDeviceContext (Xenia xam_table.inc:317)
- xboxkrnl.exe 642 (0x282) XInputdFFGetDeviceInfo (Xenia xboxkrnl_table.inc:656)
- xboxkrnl.exe 643 (0x283) XInputdFFSetEffect (Xenia xboxkrnl_table.inc:657)
- xboxkrnl.exe 644 (0x284) XInputdFFUpdateEffect (Xenia xboxkrnl_table.inc:658)
- xboxkrnl.exe 645 (0x285) XInputdFFEffectOperation (Xenia xboxkrnl_table.inc:659)
- xboxkrnl.exe 646 (0x286) XInputdFFDeviceControl (Xenia xboxkrnl_table.inc:660)
- xboxkrnl.exe 647 (0x287) XInputdFFSetDeviceGain (Xenia xboxkrnl_table.inc:661)
- xboxkrnl.exe 648 (0x288) XInputdFFCancelIo (Xenia xboxkrnl_table.inc:662)
- xboxkrnl.exe 649 (0x289) XInputdFFSetRumble (Xenia xboxkrnl_table.inc:663)

Other direct XexGetProcedureAddress call sites (6) take the ordinal in a register: not enumerable statically;
any ordinal not listed stops by name (7).
