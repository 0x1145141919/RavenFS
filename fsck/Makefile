CXX      ?= g++
CXXFLAGS  = -std=c++20 -O2 -g -Wall -Wextra
CPPFLAGS  = -I../include
LDLIBS    = -luuid
LDFLAGS   = -pthread

TARGET    = ravenfs.fsck

FSCK_OBJ  = fsck.o
GOD_DIR   = ../Userspace_God
GOD_LIB   = ../build/Userspace_God/libravenfs_god.a

.PHONY: all clean god_lib

all: $(TARGET)

$(TARGET): main.o $(FSCK_OBJ) $(GOD_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ main.o $(FSCK_OBJ) $(GOD_LIB) $(LDLIBS) $(LDFLAGS)

fsck.o: fsck.cpp fsck.hpp ../include/disk_struct.hpp ../include/magics.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

main.o: main.cpp fsck.hpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

$(GOD_LIB): god_lib

god_lib:
	$(MAKE) -C ../build ravenfs_god

clean:
	rm -f *.o $(TARGET)

.PHONY: all clean god_lib
