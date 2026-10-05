CXX      ?= g++
CXXFLAGS ?= -Wall -Wextra -O2 -g
CPPFLAGS  = -I../../include

TARGET    = sbinfo
SRCS      = sbinfo.cpp sbinfo_common.cpp sbinfo_v1.cpp sbinfo_v20.cpp

all: $(TARGET)

$(TARGET): $(SRCS) ../../include/disk_struct_v1.2.h ../../include/disk_struct_v2.0.h ../../include/magics.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
