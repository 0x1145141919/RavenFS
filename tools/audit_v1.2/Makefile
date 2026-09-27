CXX      ?= g++
CXXFLAGS ?= -Wall -Wextra -O2 -g
CPPFLAGS  = -I../../include
LDLIBS    = -lxxhash

TARGET    = audit_v1.2
SRCS      = audit_v1.2.cpp

all: $(TARGET) mkcontent mkbigdir dents_dump mkstates readback mkstates_w mkstates_t mkstates_a m44_attrcmp

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

# M4.1 只读靶子构造器（三态 / >4K / 稀疏洞 / 2 级 extent 树；写副本 + 出 .expect）
mkstates: mkstates.cpp m41_expect.h ../../include/disk_struct_v1.2.h ../../include/magics.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ mkstates.cpp $(LDLIBS)

# M4.1 回放/校验器（dump 期望内容 / verify 镜像；host 独立于 ko）
readback: readback.cpp m41_expect.h ../../include/disk_struct_v1.2.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ readback.cpp $(LDLIBS)

# M4.2 写靶子构造器（fresh/inline/direct/extent+洞；写副本 + 出 .expect/.wexpect/.plan）
mkstates_w: mkstates_w.cpp m42_fill.h m41_expect.h ../../include/disk_struct_v1.2.h ../../include/magics.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ mkstates_w.cpp $(LDLIBS)

# M4.3 靶子构造器（deep 多级树 / punch / truncate / fallocate；写副本 + 出 .expect/.wexpect/.plan）
mkstates_t: mkstates_t.cpp m42_fill.h m41_expect.h ../../include/disk_struct_v1.2.h ../../include/magics.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ mkstates_t.cpp $(LDLIBS)

# M4.4 setattr 靶子构造器（chmod/chown/utimes/truncate 靶子：REG/DIR/LNK；写副本 + 出 .expect/.wexpect/.plan/.cexpect）
mkstates_a: mkstates_a.cpp m42_fill.h m41_expect.h ../../include/disk_struct_v1.2.h ../../include/magics.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ mkstates_a.cpp $(LDLIBS)

# M4.4 host 侧属性断言小工具（离线读 inode 字段：mode/uid/gid/times/size/kind）
m44_attrcmp: m44_attrcmp.cpp ../../include/disk_struct_v1.2.h ../../include/magics.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ m44_attrcmp.cpp

clean:
	rm -f $(TARGET) mkcontent mkbigdir dents_dump mkstates readback mkstates_w mkstates_t mkstates_a m44_attrcmp

.PHONY: all clean
