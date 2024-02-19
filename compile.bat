
rem assemble kprintf.asm: vasmm68k_mot -m68000 -Fhunk -nowarn=2064 -quiet kprintf.asm -I "D:\DaynaChain\amiga-sdk-master\sdkinclude" -o kprintf.o
rem set SDK="D:\amigaxcompile\vbcc\targets\m68k-amigaos\include"
del *.o /q
del *.device /q
cls
make
