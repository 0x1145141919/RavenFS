#include "../include/ravenfs/ravenfs_v1.1.hpp"
#include "../include/ravenfs/inode_cache_radix.hpp"   /* Inode_cache_cfg_t_v2：编排构造（头里仅前置声明） */
#include "../include/ravenfs/mdata_piece_caches.hpp"  /* mdata_piece_caches_cfg_t：编排构造 */
#include "../include/ravenfs/bitmap_slices.hpp"  /* bitmap_slices_cfg_t / bitmap_slices_mgr_t：位图模块（2026-09-03 顶出） */
RavenFS_juvenile::regular_file_type_t RavenFS_juvenile::ftype(raven_Inode_t *inode)
{
    if(inode->size<=160){
        return regular_file_type_t::inline_file;
    }else{
        if(inode->is_data_root_direct_bit){
            return regular_file_type_t::direct;
        }else{
            return regular_file_type_t::extent;
        }
    }
}
result_code_t RavenFS_juvenile::direct_file_pwrite
    (uint64_t inode_idx, 
    uint64_t offset, 
    uint64_t len, 
    void *buf)
{
    //采取元数据规划
    regular_file_type_t pre_file_state;
    uint64_t data_root;
    {
        InodeSession f(this->inode_cache_mgr,inode_idx);
        //边界条件错误处理
        pre_file_state=ftype(f.inode);
        data_root=f.inode->data_root;
    }
    //接下来就是复杂的从pre_file_state状态转变了，虽然是状态转变但是是有副作用的状态转变，并且要考虑中间产生了位图资源用尽的回退这种边界条件，以及普通回滚
    //很明显初始条件是inline/direct/extent三种，我的推演发现在这个接口里面要考虑以下几个状态转变可能，但是很明显需要中间边分配边记录，最后成定论是什么状态
    //1.inline->inline态，最简单
    //2.inline->direct,扩容的时候优先尝试这个路径，hard_alloc
    //3.inline->extent,当inline->direct的升格超过2个FS_Block的时候才允许降级到这个，使用soft_alloc(笑死，一个块的时候没法撕成两个FS_block,若是一个块都没法hard_alloc出来只能说明满了)
    //4.direct->direct,不触发分配最好，触发了分配尽可能尝试调用block_occupy_soft尾部占领，尾部占领成功自然是皆大欢喜
    //5.direct->extent, 4的软占领有剩余降级这个路径
    //6.extent->extent, 待写区间落到extents上，若有空洞，尽可能从左侧区间尾巴上向右soft_occupy,能合并就合并
    //7.extent->direct,虽然极为罕见但是也有可能6的合并全文件合并到一个extent了，按照尽可能direct的宗旨就要坍缩到direct
    //不妨根据最复杂的extent初始条件的工作流程来写这个函数的五步骤：资源预分配并区间合并->根据老的extents编译一个原语操作序列->执行操作序列(map/unmap)->文件数据写入->元数据回写
    //很明显有些相对简单的边界条件可以适当简化省略对应的phase
    return result_code_t();
}