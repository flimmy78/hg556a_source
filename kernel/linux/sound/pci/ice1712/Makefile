#
# Makefile for ALSA
# Copyright (c) 2001 by Jaroslav Kysela <perex@suse.cz>
#

snd-ice17xx-ak4xxx-objs := ak4xxx.o
snd-ice1712-objs := ice1712.o delta.o hoontech.o ews.o
snd-ice1724-objs := ice1724.o amp.o revo.o aureon.o

# Toplevel Module Dependency
obj-$(CONFIG_SND_ICE1712) += snd-ice1712.o snd-ice17xx-ak4xxx.o
obj-$(CONFIG_SND_ICE1724) += snd-ice1724.o snd-ice17xx-ak4xxx.o
