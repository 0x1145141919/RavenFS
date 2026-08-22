#include "disk_struct_v1.1.hpp"
class Inode_cache_mgr_t;
class RavenFS_juvenile;
enum cache_ctrl_state_t{
    idle,
    io_ing,
    working
};
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
/**
 * 我们主要根据bits_len_of_inodes_count在1～52的枚举值动态枚举规划，[5,12]是全量上内存，[13,18]是全量缓存行控制器数组(每个都是4k页面16entries)，大于等于19才是需要引入静态基数树分片
 */
uint8_t **statey_array={
    nullptr,
};
uint8_t statey_19[]={8,7};
uint8_t statey_20[]={9,7};
uint8_t statey_21[]={9,8};
uint8_t statey_22[]={9,9};
uint8_t statey_23[]={9,10};
uint8_t statey_24[]={9,11};
uint8_t statey_25[]={9,11};
uint8_t statey_26[]={8,9,5};
uint8_t statey_27[]={8,9,6};
uint8_t statey_28[]={8,9,7};
uint8_t statey_29[]={8,10,7};
uint8_t statey_30[]={9,10,7};
uint8_t statey_31[]={9,10,8};
uint8_t statey_32[]={9,10,9};
uint8_t statey_33[]={9,10,9};
uint8_t statey_34[]={7,9,9,5};
uint8_t statey_35[]={7,9,9,6};
uint8_t statey_36[]={7,9,9,7};
uint8_t statey_37[]={8,9,9,7};
uint8_t statey_38[]={8,10,9,7};
uint8_t statey_39[]={8,10,10,7};
uint8_t statey_40[]={8,10,10,8};
uint8_t statey_41[]={8,10,10,9};
uint8_t statey_42[]={9,10,10,9};
uint8_t statey_43[]={9,10,11,9};
uint8_t statey_44[]={9,11,11,9};
uint8_t statey_45[]={7,9,9,9,7};
uint8_t statey_46[]={8,9,9,9,7};
uint8_t statey_47[]={8,9,9,10,7};
uint8_t statey_48[]={8,9,10,10,7};
uint8_t statey_49[]={8,10,10,10,7};
uint8_t statey_50[]={9,10,10,10,7};
uint8_t statey_51[]={9,10,10,11,7};
uint8_t statey_52[]={9,10,11,11,7};
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