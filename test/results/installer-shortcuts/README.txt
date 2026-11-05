Installer tool + uninstall shortcuts - verification (QEMU)
==========================================================

Program Manager path (NT 3.1, GetVersion major==3)
--------------------------------------------------
nt31-progman-group-redX.png
  After running C:\WINSIMD\WINSIMD.EXE (install): Program Manager shows a
  new "WINSIMD (Common)" group with two items:
    - "WINSIMD SIMD Info"  (the WINSINFO tool, default EXE icon)
    - "Uninstall WINSIMD"  (runs WINSIMD.EXE remove) carrying the generated
      red-X icon (UNINST.ICO, 32x32 16-color classic .ico).
  The group is created via DDE to PROGMAN:
    [CreateGroup(WINSIMD)] [ShowGroup(WINSIMD,1)]
    [AddItem(...\WINSINFO.EXE,WINSIMD SIMD Info)]
    [AddItem(...\WINSIMD.EXE remove,Uninstall WINSIMD,...\UNINST.ICO,0)]

nt31-after-uninstall.png
  After launching the "Uninstall WINSIMD" item: the group is gone
  ([DeleteGroup(WINSIMD)]) and the driver/service removed.

Explorer path (Windows 95 RTM, Add/Remove Programs)
---------------------------------------------------
win95-startmenu-lnk.png
  Start > Programs shows "WINSIMD SIMD Info", a real .lnk created via
  IShellLink/IPersistFile. ole32 is bound at runtime (NT 3.1 has no
  ole32.dll; Win95 does). The Start-Menu folder is read from the registry
  "Shell Folders" key, so there is no shell32 4.71 / IE4 dependency.

Offline confirmation (win95 SYSTEM.DAT + FAT):
  HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\WINSIMD
    DisplayName     = WINSIMD SIMD driver
    UninstallString = C:\WINDOWS\WINSIMD\WINSIMD.EXE remove
    DisplayIcon     = C:\WINDOWS\WINSIMD\WINSIMD.EXE
    Publisher       = WINSIMD
  HKLM\System\CurrentControlSet\Services\VxD\WINSIMD StaticVxD = winsimd.vxd
  C:\WINDOWS\WINSIMD\ = WINSINFO.EXE, WINSIMD.EXE, UNINST.ICO
  C:\WINDOWS\Start Menu\Programs\WINSIMD SIMD Info.lnk (322 bytes)
  C:\WINDOWS\SYSTEM\WINSIMD.VXD

Shell/uninstall selection rule (installer/shortcuts.c):
  major==3 (Program Manager, no Add/Remove) -> DDE group, uninstall as a
    group item with the red-X icon.
  else (Win9x/NT4+/2000/XP, Explorer + Add/Remove) -> Start-Menu .lnk for
    the tool, uninstall registered in Add/Remove Programs (no shortcut).
