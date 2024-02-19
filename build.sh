export VBCC=../../vbcc
export PATH=$PATH:$VBCC/bin
export SDK="-I$VBCC/targets/m68k-amigaos/include -I$VBCC/targets/m68k-amigaos/include2"

make
