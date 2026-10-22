TSReader_FutureTel.dll: loading it launches the old WinOPTI kernel-driver installer
("Winopti Error" prompt) every time the source list is built. FutureTel cards
and that driver do not work on 64-bit Windows 10/11. Move it back to Sources\r
only if you really have that hardware on an old 32-bit system.
