CFLAGS ?= -Wall -O2
FRAMEWORKS = -framework IOKit -framework CoreFoundation
all: qcabt usbdesc
qcabt: qcabt.c
	clang $(CFLAGS) -o $@ $< $(FRAMEWORKS)
usbdesc: usbdesc.c
	clang $(CFLAGS) -o $@ $< $(FRAMEWORKS)
clean:
	rm -f qcabt usbdesc
