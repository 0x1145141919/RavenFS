CXX      ?= c++
CXXFLAGS ?= -Wall -Wextra -O2 -g
CPPFLAGS  = -I../include
LDLIBS    = -luuid

TARGET    = mkfs.ravenfs
SRCS      = main.cpp mkfs.cpp
OBJS      = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

HEADERS = mkfs.hpp ../include/disk_struct_v1.1.hpp ../include/magics.h

%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
