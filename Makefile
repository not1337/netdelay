# This file is part of the netdelay project
# 
# (C) 2020 Andreas Steinmetz, ast@domdv.de
# The contents of this file is licensed under the GPL version 2 or, at
# your choice, any later version of this license.
#
OPTS=
#
# Enable the following for Raspberry Pi 4B
#
# OPTS=-march=native -mthumb -fomit-frame-pointer -fno-stack-protector

all: netdelay

netdelay: netdelay.c
	gcc -Wall -O3 $(OPTS) -s -o netdelay netdelay.c

clean:
	rm -f netdelay
