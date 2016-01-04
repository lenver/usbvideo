
CFLAGS	+= -I$(SDKTARGETSYSROOT)/usr/include/gstreamer-1.0
CFLAGS	+= -I$(SDKTARGETSYSROOT)/usr/include/glib-2.0
CFLAGS	+= -I$(SDKTARGETSYSROOT)/usr/lib/glib-2.0/include
CFLAGS	+= -I/home/lenver/work/mxclibs/include

LDFLAGS	+= -L/home/lenver/work/mxclibs/lib
LDFLAGS	+= -lg2d -lfslvpuwrap -lvpu

TARGET1	:= uvcenc
TARGET2	:= uvcdec

TARGETS := $(TARGET1)

all:$(TARGETS)

$(TARGET1):$(TARGET1).o vpuenc.o
	$(CC) -o $@ $^ $(LDFLAGS)
	$(STRIP) $@

$(TARGET2):$(TARGET2).o vpudec.o
	$(CC) -o $@ $^ $(LDFLAGS)
	$(STRIP) $@

%.o:%.cpp
	$(CC) -c -o $@ $< $(CFLAGS)

%.o:%.c
	$(CC) -c -o $@ $< $(CFLAGS)

clean:
	@rm -rf *.o $(TARGETS)
