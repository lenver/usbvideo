#
#

CFLAGS	+= -I/home/lenver/work/mxclibs/include
CXXFLAGS+= -I/home/lenver/work/mxclibs/include

LDFLAGS	+= -L/home/lenver/work/mxclibs/lib
LDFLAGS	+= -lg2d -lfslvpuwrap -lvpu


TARGET1 := uvcdec
TARGET2 := uvcenc
TARGET3 := filedec
TARGET	:= $(TARGET1) $(TARGET2) $(TARGET3)


.PHONY:all
all:$(TARGET)

$(TARGET1):$(TARGET1).o vpudec.o
	$(CC) $^ -o $@ $(LDFLAGS)
	$(STRIP) $@

$(TARGET2):$(TARGET2).o vpuenc.o
	$(CC) $^ -o $@ $(LDFLAGS)
	$(STRIP) $@

$(TARGET3):$(TARGET3).o vpudec.o
	$(CC) $^ -o $@ $(LDFLAGS)
	$(STRIP) $@

%.o:%.c
	$(CC) $(CFLAGS) -c $^ -o $@

%.o:%.cpp
	$(CXX) $(CXXFLAGS) -c $^ -o $@

.PHONY:clean
clean:
	@rm -rf *.o *.d *.d.* $(TARGET)

