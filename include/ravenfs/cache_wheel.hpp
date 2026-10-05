#pragma once
#include "rc.hpp"        /* result_code_t / RC_*（轻量头，零盘结构依赖） */
#include <cstdint>

/* ═══════════════════════════════════════════════════════════════════
 * cache_wheel.hpp — 缓存通用轮子（inode 缓存 / mdata_piece 缓存共用）
 *
 * 2026-10-05 从 inode_cache_radix.hpp 顶部抽出：inode 缓存与 mdata_piece 缓存
 * 共用同一套「ctrl 状态机 + 位域分片解码」原语。抽出的动机 = 解包含环——
 *   · mdata_piece_caches.hpp 需要这组轮子（CTRL_* / bitfield decode）；
 *   · 而 inode 侧的 InodeSession 现在（v2.0）要内嵌 mdata_piece_session，
 *     于是 inode_cache_radix.hpp 必须 include mdata_piece_caches.hpp；
 *   · 若轮子仍住 inode_cache_radix.hpp，则两文件互相 include 成环。
 * 故把两缓存共用的设计（与任何一端的具体缓存类型无关）下沉到本头，两侧各自
 * include 本头即可，包含图倒为无环：
 *
 *     cache_wheel.hpp  ←  mdata_piece_caches.hpp
 *                      ←  inode_cache_radix.hpp（InodeSession/旧 inode 缓存）
 * ═══════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════
 * ctrl_word 单原子字位域布局（state + flags + accessor 打包）：
 *   [0,8)   state    —— CAS 状态机（idle/io_ing/working/evicting）
 *   [8,24)  flags    —— 修饰位（CTRL_FLAG_DIRTY）
 *   [24,56) accessor —— 会话占坑计数（32 位）
 *
 * 为什么打包：reap「accessor==0 才能接管淘汰」与 get「占坑 +accessor」
 * 必须落在同一个原子操作上，否则存在 TOCTOU：
 *   旧两字段方案：get 复核 state==working 通过后返回指针，reap 才 CAS
 *   working→evicting 成功（其 accessor 检查读在 get 占坑之前）→ 页被 free，
 *   已返回的指针悬垂。
 * 打包后：占坑 = CAS(word, word + INC)（成功即占坑，无需复核）；
 *         接管 = CAS(word, evicting)（CAS 前提含 working && accessor==0）
 *         两个窗口同时关闭。
 * ═══════════════════════════════════════════════════════════════════ */
enum cache_ctrl_state_t{
    idle,      /* 未载入（CAS 状态机：idle → io_ing → working） */
    io_ing,    /* 他人加载中 / 本线程持有加载权 */
    working,   /* 已就绪，可被占坑 */
    evicting   /* TTL 淘汰接管中：写回+释放进行时，get 忙等，完成后回 idle */
};
static constexpr uint64_t CTRL_STATE_MASK     = 0xFFull;
static constexpr uint64_t CTRL_FLAGS_SHIFT    = 8;
static constexpr uint64_t CTRL_ACCESSOR_SHIFT = 24;
static constexpr uint64_t CTRL_ACCESSOR_INC   = 1ull << CTRL_ACCESSOR_SHIFT;
static constexpr uint64_t CTRL_FLAG_DIRTY     = 1u << 0;   /* flags 域内位：页被修改，需回写 */

struct cut_result_t{
    uint64_t main;
    uint64_t cut_out_section;
};
extern cut_result_t cut_high(uint64_t raw_metarial,uint8_t bits_to_cut);
/* cut_high — 位域切割原语（左对齐方案，通用轮子，块缓存等场景可复用）：
 * raw 须已左对齐（位域最高位 = bit 63）；从位域顶端向下切 bits_to_cut 个 bit：
 * cut_out_section = 切下的高位段（右对齐），main = 剩余位域（保持左对齐，供递归）。
 * 实现见 juvenile_god/inode_cache.cpp */

/* ═══════════════════════════════════════════════════════════════════
 * 位域分片规划/解码 —— 通用轮子（inode 缓存与 mdata_piece 缓存共用）
 *
 * bitfield_decode_plan_t：按 bits_len 从最高位向下逐层切分（cut_high 递归），
 *   level_bits[0] = mgr 层分片选择位宽（slice 数 = 2^w0），level_bits[1..]
 *   = 基数树逐层位宽，剩余低位 = 缓存行内引索（row_bits = bits - Σlevel_bits）。
 *   行内宽度由 decode 派生，plan 为唯一真相源，decode 不依赖 mgr 状态。
 * bitfield_decoded_result_t：chunks[0..total_levels-1] = 各层索引（[0]=slice 选择），
 *   chunks[total_levels] = 行内索引；行内位宽 = 0 时该槽恒 0（整行 = 1 单元）。
 * decode 实现见 juvenile_god/inode_cache.cpp（static 全局，切片树可直接复用）。
 * ═══════════════════════════════════════════════════════════════════ */
struct bitfield_decode_plan_t{
    /**
     * bits_len：索引总位宽（= ceil(log2(count))，plan 必须自包含）：
     *           decode 左对齐 + row_bits 派生的唯一真相源
     * total_levels：使用的层数（含 mgr 层），level_bits[0..total_levels-1] 有效
     * level_bits[0]：mgr 层分片选择位宽（slice 数 = 2^level_bits[0]）
     * level_bits[1..]：分片基数树从上到下的逐层位宽
     * 剩余低位 = 行内引索：row_bits = bits_len - sum(level_bits)
     */
    uint8_t bits_len;
    uint8_t total_levels;
    uint8_t*level_bits;
};
struct bitfield_decoded_result_t{
    uint64_t raw;//原始数据（索引）
    uint8_t total_levels;//从plan处继承：0～total_levels-1 引索中间位域解析结果（层级索引），引索 total_levels 的是缓存行解码引索（行内索引）
    uint16_t chunks[7];//层级索引容量：现表最大 5 层 + 行内 = 6，留 1 冗余（支撑未来 6 层）；每项位宽 ≤ 11bit ≤ uint16_t
};
extern bitfield_decoded_result_t decode(uint64_t raw, bitfield_decode_plan_t plan);
/* decode — 按 plan 逐层切分索引位域（实现见 inode_cache.cpp）。
 * chunks[0..total_levels-1] = 各层索引（chunks[0] = mgr 层 slice 选择），
 * chunks[total_levels] = 行内索引。row_bits 由 plan 累积派生（不依赖外部常量），
 * plan 为唯一真相源；raw 内部左对齐到位域顶端后递归 cut_high。
 * 约束：plan.total_levels ≤ 6（chunks[7] 容量，现表最大 5 层）。 */
