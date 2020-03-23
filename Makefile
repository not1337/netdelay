# This file is part of the netdelay project
# 
# (C) 2020 Andreas Steinmetz, ast@domdv.de
# The contents of this file is licensed under the GPL version 2 or, at
# your choice, any later version of this license.
#
all: netdelay

netdelay: netdelay.c
	gcc -Wall -O3 -s -o netdelay netdelay.c

clean:
	rm -f netdelay
