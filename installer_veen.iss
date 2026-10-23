; TSReader_Veen installer (Inno Setup 6)
; Build:  "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" installer_veen.iss
; Output: installer_output\TSReader_Veen_Setup.exe
;
; Based on installer.iss (TSReaderPro memorial build) plus the Veen additions:
; embedded ffplay player (with the VC++ runtime it needs), DekTec DTU-236 sources,
; receiver-control plugins, the hardware SDK DLLs used by source plugins, and the
; modules from Sources_Archive. Left out on purpose:
;   - TSReader_FutureTel.dll / OptiInstall.dll / ftusdk.dll: loading the FutureTel source
;     starts the WinOPTI driver installer every time the source list is built
;   - msvcrt.dll: a Windows system file, never redistributed
;   - CSAnew.dll, NagraDES.dll, MDPlugins: conditional-access decryption modules
;   - DekTec drivers: install DekTec's Dtu driver package separately when needed

#define AppName "TSReader_Veen"
#define AppVersion "2.8.53h-veen"
#define VcRedist "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC\14.51.36231\x64\Microsoft.VC145.CRT"

[Setup]
AppId={{E7E717F4-7EA1-48EB-90A9-B2266CDC2FB3}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=SB O-ou
AppPublisherURL=https://github.com/vommi/TSReader
AppSupportURL=https://github.com/vommi/TSReader
DefaultDirName={commonpf32}\{#AppName}
DefaultGroupName={#AppName}
OutputDir={#SourcePath}\installer_output
OutputBaseFilename=TSReader_Veen_Setup
Compression=lzma2
SolidCompression=yes
SetupIconFile={#SourcePath}\dvb.ico
UninstallDisplayIcon={app}\dvb.ico
; TSReader is 32-bit; the bundled ffplay is 64-bit and needs 64-bit Windows
ArchitecturesAllowed=x64compatible
WizardStyle=modern
LicenseFile={#SourcePath}\LICENSE
PrivilegesRequired=admin
DisableDirPage=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "finnish"; MessagesFile: "compiler:Languages\Finnish.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Main executable and the SourceHelper it was built with
Source: "{#SourcePath}\build\Release\TSReaderPro.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\TSReaderPro.exe.manifest"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\build\Release\TSReader_SourceHelper.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\build\Release\libfaad2.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\build\Release\PEGRPCS.DLL"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\_ISource.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\pthreadVSE2.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\adpsi30.dll"; DestDir: "{app}"; Flags: ignoreversion

; TSReader helper plugins
Source: "{#SourcePath}\TSReader_MPEG4.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\TSReader_H264.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\TSReader_ForVid.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\TSReader_Scheduler.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\TSReader_VLC.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\TSReader_XNS.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\TSReader_DVHS.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\TSReader_UDPSender.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\TSReader_Stradis.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\TSReader_Stradis12.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\TSReader_Stradis16.dll"; DestDir: "{app}"; Flags: ignoreversion

; Hardware SDK DLLs used by source plugins, and the old runtimes they need
Source: "{#SourcePath}\ATDV_API2.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\Board1049.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\Board1077.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\RtdBin.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\RtdSdk.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\ThDemul.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\ThDevice.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\TSPLLD0421.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\ttlcdacc.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\ttusb2acc.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\MFC71.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\msvcr70.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\msvcr71.dll"; DestDir: "{app}"; Flags: ignoreversion

; Helper executables
Source: "{#SourcePath}\vc1-decoder.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\TSReader_ArchiveMonitor.exe"; DestDir: "{app}"; Flags: ignoreversion

; Images, sounds, data
Source: "{#SourcePath}\*.bmp"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\*.png"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\archive-siren.wav"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\ndscam.dat"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\dvb.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "{#SourcePath}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\*.tmc"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourcePath}\freesat.t1"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourcePath}\freesat.t2"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

; Config and preset lists - user-writable so tune dialogs can save under Program Files
Source: "{#SourcePath}\BISS.ini"; DestDir: "{app}"; Flags: ignoreversion; Permissions: users-modify
Source: "{#SourcePath}\diseqcU.ini"; DestDir: "{app}"; Flags: ignoreversion; Permissions: users-modify
Source: "{#SourcePath}\dvbt.ini"; DestDir: "{app}"; Flags: ignoreversion; Permissions: users-modify
Source: "{#SourcePath}\S2emu.ini"; DestDir: "{app}"; Flags: ignoreversion; Permissions: users-modify
Source: "{#SourcePath}\*.lst"; DestDir: "{app}"; Flags: ignoreversion; Permissions: users-modify

; Source plugins: Sources\ (incl. the DekTec DTU-236 QAM/ASI sources) and Rod's later
; builds from Sources_Archive, flattened into one folder
Source: "{#SourcePath}\Sources\*.dll"; DestDir: "{app}\Sources"; Flags: ignoreversion
Source: "{#SourcePath}\Sources_Archive\*.dll"; DestDir: "{app}\Sources"; Flags: ignoreversion
Source: "{#SourcePath}\Sources\ReceiverControl\*.dll"; DestDir: "{app}\Sources\ReceiverControl"; Flags: ignoreversion

; Forwarders and satellite data
Source: "{#SourcePath}\Forwarders\*.dll"; DestDir: "{app}\Forwarders"; Flags: ignoreversion
Source: "{#SourcePath}\Satellites\*.ini"; DestDir: "{app}\Satellites"; Flags: ignoreversion; Permissions: users-modify

; DVB-C channel plans for the tune dialog's Plan list (user-writable: NIT > Save as plans writes here)
Source: "{#SourcePath}\CablePlans\*.lst"; DestDir: "{app}\CablePlans"; Flags: ignoreversion; Permissions: users-modify

; Embedded player: ffplay (LGPL FFmpeg build) + SDL2 + the VC++ runtime it needs, app-local
Source: "{#SourcePath}\ffplay\*"; DestDir: "{app}\ffplay"; Flags: ignoreversion
Source: "{#VcRedist}\vcruntime140.dll"; DestDir: "{app}\ffplay"; Flags: ignoreversion
Source: "{#VcRedist}\vcruntime140_1.dll"; DestDir: "{app}\ffplay"; Flags: ignoreversion

[Dirs]
Name: "{app}\CablePlans"; Permissions: users-modify

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\TSReaderPro.exe"; IconFilename: "{app}\dvb.ico"; WorkingDir: "{app}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\TSReaderPro.exe"; IconFilename: "{app}\dvb.ico"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\TSReaderPro.exe"; Description: "Launch {#AppName}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent
