CXX      ?= c++
CXXFLAGS ?= -Wall -Wextra -O2 -g
CPPFLAGS  = -I../include
LDLIBS    = -luuid

TARGET    = fsck.ravenfs
SRCS      = main.cpp fsck.cpp
OBJS      = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

HEADERS = fsck.hpp ../include/disk_struct.hpp ../include/magics.h

%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
