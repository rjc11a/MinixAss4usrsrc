# Makefile for terminal driver (TTY)
DRIVER = tty

include /etc/make.conf

# directories
u = /usr
i = $u/include
s = $i/sys
m = $i/minix
b = $i/ibm
d = ..

# programs, flags, etc.
MAKE = exec make
CC =	exec cc
CPPFLAGS = -I$i -I../../kernel/arch/$(ARCH)/include
CFLAGS = $(CPPFLAGS)
LDFLAGS = -i
LIBS = -lsysutil -lsys -ltimers

OBJ = tty.o console.o vidcopy.o keyboard.o pty.o rs232.o 

# build local binary 
all build:	$(DRIVER)
$(DRIVER):	$(OBJ) 
	$(CC) -o $@ $(LDFLAGS) $(OBJ) $(LIBS)
	install -S 8k $(DRIVER)

# install with other drivers
install:	
	cd keymaps && $(MAKE) -$(MAKEFLAGS) install

#/sbin/$(DRIVER):	$(DRIVER)
#	install -o root -cs $? $@

# clean up local files
clean:
	cd keymaps && $(MAKE) -$(MAKEFLAGS) $@
	rm -f $(DRIVER) *.o *.bak 

depend: 
	mkdep "$(CC) -E $(CPPFLAGS)" *.c > .depend

# Include generated dependencies.
include .depend


