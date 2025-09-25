###############################################################################
#
# makefile for vbcc or gcc
#
# original author: Henryk Richter <henryk.richter@gmx.net>
# 
# concept:
#
# tools required:
#  - vbcc, defaulting to m68k-amigaos
#  - vlink
#  (- vasm)
#
# porting:
#
#  see Common.mk
#
###############################################################################

###############################################################################
# Date, version, extra objects to build
# 
###############################################################################
DEVICEVERSION=1
DEVICEREVISION=3
DEVICEDATE=2025-09-25

###############################################################################
# Devices to build (1 or 2, keep DEVICEID2 empty if only one build is desired)
#
###############################################################################

DEVICEID=scsidayna.device
DEFINES = #
ASMDEFS = #
CPU = 68000

DEVICEID2=

###############################################################################
# import generic ruleset
# 
###############################################################################
include Common.mk



