CXX      ?= g++
CXXFLAGS ?= -Wall -Wextra -O2 -g
CPPFLAGS  = -I../../include
LDLIBS    = -lxxhash

TARGET    = audit_v1.2
SRCS      = audit_v1.2.cpp

all: $(TARGET) mkcontent mkbigdir dents_dump

$(TARGET): $(SRCS) ../../include/disk_struct_v1.2.h ../../include/magics.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ $(SRCS) $(LDLIBS)

# 仅供测试的最小内容填充器（写副本；auditor 自身不写盘）
mkcontent: mkcontent.cpp ../../include/disk_struct_v1.2.h ../../include/magics.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ mkcontent.cpp $(LDLIBS)

# 大目录靶子构造器（多叶/多层 dentry 树 + 双向叶链；写副本）
mkbigdir: mkbigdir.cpp ../../include/disk_struct_v1.2.h ../../include/magics.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ mkbigdir.cpp $(LDLIBS)

# getdents64 续读压测/枚举器（纯 libc；在 guest 里跑）
dents_dump: dents_dump.cpp
	$(CXX) $(CXXFLAGS) -o $@ dents_dump.cpp

clean:
	rm -f $(TARGET) mkcontent mkbigdir dents_dump

.PHONY: all clean
