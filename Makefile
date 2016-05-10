#
CC=		x86_64-ucb-akaros-gcc
CFLAGS=		-Wall -Wno-format -O2 -I.
LIBS=		-lndblib -liplib -lbenchutil
TARG=		ipconfig

OFILES=		main.o ipv6.o
HFILES=		dhcp.h icmp.h ipconfig.h

$(TARG):	$(OFILES)
		$(CC) -o $(TARG) $(OFILES) $(LIBS)

%.o:		%.c $(HFILES)
		$(CC) $(CFLAGS) -c $*.c

clean:
		rm -f *.o $(TARG)

.PHONY:		clean install
