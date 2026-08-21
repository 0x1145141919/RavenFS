#include "disk_struct_v1.1.hpp"
class Inode_cache_mgr_t;
class RavenFS_juvenile;
struct Inode_cache_ctrl_t{
    raven_Inode_t*cache_data;
    uint64_t accss_stamp;
    uint32_t numof_accessor;
    uint16_t flags;
    uint8_t state;//需要以cas状态切换的
};
struct cut_result_t{
    uint64_t main;
    uint64_t cut_out_section;
};
extern cut_result_t cut_high(uint64_t raw_metarial,uint8_t bits_to_cut);//从 raw_metarial的最高非0有效位开始向下切bits_to_cut个bit,切下来的放入cut_out_section，原主干放入main
class Inode_cache_mgr_t{
    Inode_cache_mgr_t();//被对应的RavenFS_juvenile构造
    ~Inode_cache_mgr_t();//每个片进行折构
    uint8_t bits_len_of_inodes_count;
    struct bits_slice_plan_t{//词不达意，我想表达的是根据bits_len_of_inodes_count的后如何规划位域对应到对应层级的表的引索，这种意思        
        /**
         * total_levels表示这个bits_len_of_inodes_count从最高位开始向下每个多少层，也就是
         * per_level_width数组从0~total_levels-1的条目是使用的
         * 而解析位域则是使用对应的cut_high函数，从原始位域递归从0迭代向上切，每切出来一份放入解析成员，最后剩下的就是缓存行内引索
         */
        /**
         * 不过先硬性定一手，per_level_width[0]固定为8，是Inode_cache_mgr_t层的引索
         */
        uint8_t total_levels;
        uint8_t per_level_width[15];
    };
    bits_slice_plan_t global_slice_plan;


};