/* ═══════════════════════════════════════════════════════════════════
 * fs_allocator.cpp — mgr_slice_t 主从红黑树分片分配器（片自治单元实现）
 *
 * 2026-10-03 新建。模块契约见 include/ravenfs/fs_allocator.hpp 头注释
 * （逐字为规格）；设计意图见 Docs/runtime_tech/04_sharded_master_slave_rbtree.md。
 *
 * 旧位图实现 bitmap_slices.cpp 只作**语义参照**（位义、hint、free_count 的
 * 增量记账口径），本文件是**红黑树版**：内部结构从「位图 + 位扫」换成
 * 「主树 by_pos + 从树 by_size」双红黑树，坐标一律全局位号、单位一律「位」。
 *
 * 本轮只实现头文件里声明的 6 个成员函数：
 *   enable_slice / test_interval / free_interval / free_intervals /
 *   all_dump / soft_occupy
 * 其余（alloc_hard / soft_alloc / shutdown / disable_slice / get_free_count）
 * 声明保留在头里，本文件不实现——不定义即不参与链接，未解析只是调用方的
 * 事（这里没有任何 TU 引用它们）。
 *
 * 锁纪律（继承头文件）：本文件所有函数**一律不加锁**，临界区由调用方用
 *   slice_lock（可睡 std::mutex）编排；片内零主动 IO。
 *
 * ── 两棵树的一致性纪律（本文件反复引用，先在此钉死）─────────────
 *   invariant（单向）：size_ 的每个节点 ∈ pos_ 的节点集合（从 ⊆ 主）。
 *   所有「挂/摘」都成对进行，且删除顺序 = 从树先、主树后；插入顺序 =
 *   主树先、从树后。这样在每个指令边界都维持「没有孤儿从树节点」：
 *     - 删：若先删主树，从树元素会悬垂（主树对象已销毁）→ 违反；
 *           先从树摘（钩子归零）再删主树，安全。
 *     - 插：先主树拿稳定地址（std::set 元素地址稳定），再把该地址挂从树；
 *           若先从树插入，得先有对象——对象正是主树元素，故必先主树。
 *   注意 std::set 迭代器/引用在 insert 下稳定（红黑树不搬元素），
 *   所以「取址挂从树」是安全的。erase 只失效被删迭代器，故删左邻后
 *   右邻迭代器仍可用。
 * ═══════════════════════════════════════════════════════════════════ */

#include "../include/ravenfs/fs_allocator.hpp"

#include <iterator>   /* std::prev */
#include <new>        /* std::bad_alloc */

/* ═══════════════════════════════════════════════════════════════
 * 内部轮子（匿名 namespace）—— 双树成对操作
 *
 * 为什么不做成 mgr_slice_t 私有成员：头文件的成员布局由设计方并行修改，
 * 本轮约定「只新建 cpp、不动头」。故这组小轮子以**自由函数**形式接收两棵
 * 树的引用（树类型是 public 别名，可外部组合），语义上仍是片内的私有操作，
 * 只是不占类成员位。命名带 _both 提醒调用方「必须成对维护从 ⊆ 主」。
 * ═══════════════════════════════════════════════════════════════ */
namespace {

/* 插入一个空闲区间：主树先（拿到稳定元素地址）、从树后（挂侵入式钩子）。
 * 返回主树迭代器（后续若要再操作该节点可用）。 */
fs_pos_tree_t::iterator insert_both(fs_pos_tree_t &pos, fs_slave_tree_t &size,
                                    uint64_t base, uint64_t len) {
    fs_interval_t fresh{};          /* 钩子默认未链接；拷贝进 set 时钩子不复制链接 */
    fresh.base = base;
    fresh.len = len;
    auto res = pos.insert(fresh);   /* 主树：唯一真相源先落位 */
    /* std::set 元素是 const；从树钩子声明 mutable。这里的 const_cast 是
     * **有界**的：只允许从树改写 by_size 钩子，绝不触碰主树序键 base。 */
    fs_interval_t &elem = const_cast<fs_interval_t &>(*res.first);
    size.insert_unique(elem);       /* 从树：派生索引后挂 */
    return res.first;
}

/* 摘除一个已双挂的空闲区间：从树先、主树后（维持「每个指令边界从 ⊆ 主」）。 */
void erase_both(fs_pos_tree_t &pos, fs_slave_tree_t &size,
                fs_pos_tree_t::iterator it) {
    fs_interval_t &elem = const_cast<fs_interval_t &>(*it);  /* 有界 const_cast，理由同上 */
    size.erase(size.iterator_to(elem));   /* 先从树：钩子归零，消掉悬垂风险 */
    pos.erase(it);                        /* 后主树：此时已无从树引用该对象 */
}

/* 清空双树：侵入从树的钩子嵌在**主树元素**里 —— 必须先 size_.clear()（摘钩，
 * 不析构元素）再 pos_.clear()（析构元素）。反序 = 从树遍历已释放节点（UAF，
 * 原型 bug#2）。enable_slice 清底/异常回滚、shutdown 一律走本函数。 */
inline void clear_both(fs_pos_tree_t &pos, fs_slave_tree_t &size) {
    size.clear();   /* 先摘从树钩子（不析构元素） */
    pos.clear();    /* 再析构主树元素 */
}

/* 构造仅用于 lower_bound / upper_bound 的探针（fs_pos_less 只看 base，
 * 其余字段无关；显式置零避免读未初始化）。 */
inline fs_interval_t make_pos_probe(uint64_t base) {
    fs_interval_t p{};
    p.base = base;
    p.len = 0;
    return p;
}

}  // namespace

/* ═══════════════════════════════════════════════════════════════
 * enable_slice — 借 config 原始位图一次性建出主从双树
 *
 * 门闸：is_workable 必须 false→true；已 true → RC_ERR_STATE（重复使能）。
 * 校验（失败保持 is_workable=false、树空）：
 *   - slice_base 非空；
 *   - slice_base 4K 对齐（与位图契约的片对齐一致，防后续按页做 checkpoint
 *     时错位）；
 *   - slice_bitscount > 0（空片无意义）。
 * 建树：第 i 位 ↔ 全局位号 (in_global_base_idx + i)；磁盘契约 1=已占用、
 *   0=空闲。逐段扫出**极大空闲 run**（连续 0），喂进双树。
 * 记账：free_count_ = Σ len（空闲位数）；last_finish_hint_ = 0（分配游标
 *   本轮不实现 alloc，但保持与新片初始化一致）。
 * 关键：本函数返回后**不再引用** config.slice_base —— 建树即消费完，
 *   配置内存可立即释放/复用（头注释的「借阅非持有」）。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t mgr_slice_t::enable_slice(slice_config_t config) {
    if (is_workable) return RC_ERR_STATE;                 /* 已使能：拒绝重复 enable */
    if (config.slice_base == nullptr) return RC_ERR_STATE;
    if (reinterpret_cast<uintptr_t>(config.slice_base) & 4095ULL)
        return RC_ERR_STATE;                              /* 非 4K 对齐 */
    if (config.slice_bitscount == 0) return RC_ERR_STATE;

    /* 清底：理论上未使能应空，但显式清可挡住「上次失败残留」这类脏态。 */
    clear_both(pos_, size_);
    in_global_base_idx = config.in_global_base_idx;       /* 内部化起点 */
    slice_bitscount_   = config.slice_bitscount;          /* 内部化位数（与起点共同界定半开区间） */
    free_count_ = 0;
    last_finish_hint_ = 0;

    const uint8_t *bits = config.slice_base;              /* 只借用，函数末尾不再引用 */

    /* 异常安全：std::set::insert 可能抛 bad_alloc；一旦抛出退回「未使能 + 空树」。 */
    try {
        uint64_t i = 0;
        while (i < config.slice_bitscount) {
            const bool occupied = (bits[i >> 3] >> (i & 7)) & 1u;
            if (occupied) { i++; continue; }              /* 1 = 已占用，跳过 */
            const uint64_t start = i;
            while (i < config.slice_bitscount &&
                   (((bits[i >> 3] >> (i & 7)) & 1u) == 0)) {
                i++;                                      /* 吞掉连续 0 = 极大空闲 run */
            }
            const uint64_t run_len = i - start;
            insert_both(pos_, size_, in_global_base_idx + start, run_len);
            free_count_ += run_len;
        }
    } catch (const std::bad_alloc &) {
        clear_both(pos_, size_);
        free_count_ = 0;
        last_finish_hint_ = 0;
        return RC_ERR_STATE;
    }

    /* bits 在此之后不再被引用（配置内存即刻可弃）。 */
    is_workable = true;
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * test_interval — 断言 [base, base+len) 全为期望态
 *
 * val == false（全空闲）：空闲区间**两两不重叠且极大**（主树不变式），
 *   所以「整段全空闲」⟺ 整段被**同一个**空闲区间覆盖。用 upper_bound(base)
 *   退一格拿到「可能包含 base 的区间」，看它能否完全罩住段尾。
 * val == true（全占用）：「段内不含任何空闲位」⟺ 没有空闲区间与段相交。
 *   相交只可能来自两处：包含 base 的前驱区间（可能从段左伸进来），或第一个
 *   base' ≥ base 的后继区间（可能落在段内）。逐一双查即可，无需逐位。
 * 序：先门闸（!is_workable → RC_ERR_STATE），再范围，再 len==0 退化，最后判定。
 *
 * 范围：本片全局半开区间 [in_global_base_idx, in_global_base_idx + slice_bitscount_)；
 *   越下界 / 越上界一律 RC_ERR_STATE（上下界均由内部化字段界定）。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t mgr_slice_t::test_interval(bool val, uint64_t base, uint64_t len) {
    if (!is_workable) return RC_ERR_STATE;
    if (base < in_global_base_idx) return RC_ERR_STATE;   /* 片首之下 = 越出本片范围 */
    {
        const uint64_t off = base - in_global_base_idx;
        /* ⚠ 先判 off 上界：base 越过片尾时 `slice_bitscount_ - off` 会无符号下溢成巨值
         *   而令检查失效（原型 bug#1）；off > slice_bitscount_ 单独挡。 */
        if (off > slice_bitscount_ || len > slice_bitscount_ - off)
            return RC_ERR_STATE;                          /* 片尾之上 = 越出本片范围 */
    }
    if (len == 0) return RC_OK;                           /* 空段恒真（退化） */

    const fs_interval_t probe = make_pos_probe(base);

    if (!val) {
        /* 期望全空闲：找「可能包含 base」的空闲区间（最后一个 base ≤ base 的）。 */
        auto it = pos_.upper_bound(probe);   /* 第一个 base > base */
        if (it == pos_.begin()) return RC_ERR_NOT_FOUND;
        --it;
        const bool covers = (it->base <= base) && (base + len <= it->base + it->len);
        return covers ? RC_OK : RC_ERR_NOT_FOUND;
    }

    /* 期望全占用：段内出现任何空闲位即 NOT_FOUND。 */
    auto it = pos_.lower_bound(probe);       /* 第一个 base ≥ base */
    if (it != pos_.end() && it->base < base + len)
        return RC_ERR_NOT_FOUND;             /* 后继区间左端落在段内 */
    if (it != pos_.begin()) {
        auto prev = std::prev(it);
        if (prev->base + prev->len > base)
            return RC_ERR_NOT_FOUND;         /* 前驱区间右端探进段内 */
    }
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * free_interval — 返还 [base, base+len)，主树重叠检测 + 邻接融合
 *
 * 位图版 free_interval 是幂等清位（重复释放不虚高）；红黑树版不同：
 * 「空闲集」是显式区间，释放一段**已经空闲**的位属于语义矛盾（重叠）——
 * 头注释要求「检测到重叠会报错，绝不静默改写」。故这里把重叠当错误。
 *
 * 重叠判定（主树不变式：相邻区间不重叠）：
 *   it = lower_bound(base)（第一个 base' ≥ base）；
 *   - 前驱 prev（最后一个 base' < base）若 prev.base+prev.len > base → 重叠；
 *   - it 自身若 it.base < base+len → 重叠。
 * 融合：纯邻接（prev.base+prev.len == base / it.base == base+len）才并；
 *   合并后区间 = [min(左base, base), max(右end, base+len))。
 *
 * 顺序（两处纪律严格照做）：
 *   先删被波及邻：从树先、主树后（erase_both 封装）；
 *   再插合并区间：主树先、从树后（insert_both 封装）。
 * 记账：free_count_ += len（重叠已排除，新增空闲恰为 len，无重复计数）。
 *
 * 范围：同 test_interval，越上下界一律 RC_ERR_STATE。
 * 重叠错误码：用全局共享的 **RC_ERR_OVERLAP**（原 extent 专用，本处按
 *   「区间重叠·契约违反」同义复用；其 disk_struct 注释已同步泛化为
 *   「区间与既有映射/空闲区重叠」——不新造码，避免 RC 分类学腐坏）。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t mgr_slice_t::free_interval(uint64_t base, uint64_t len) {
    if (!is_workable) return RC_ERR_STATE;
    if (base < in_global_base_idx) return RC_ERR_STATE;
    {
        const uint64_t off = base - in_global_base_idx;
        /* ⚠ 先判 off 上界（同 test_interval 的 bug#1）：base 越过片尾时
         *   `slice_bitscount_ - off` 无符号下溢 → 检查失效。 */
        if (off > slice_bitscount_ || len > slice_bitscount_ - off)
            return RC_ERR_STATE;                           /* 片尾之上 = 越出本片范围 */
    }
    if (len == 0) return RC_OK;                            /* 空释放：无操作 */

    const fs_interval_t probe = make_pos_probe(base);
    auto it = pos_.lower_bound(probe);                     /* 第一个 base' ≥ base */
    const bool has_left = (it != pos_.begin());
    auto left_it = has_left ? std::prev(it) : pos_.end();

    /* ── 重叠检测（守卫主树不变式）── */
    if (has_left && left_it->base + left_it->len > base)
        return RC_ERR_OVERLAP;                             /* 左邻伸进释放区间（契约违反） */
    if (it != pos_.end() && it->base < base + len)
        return RC_ERR_OVERLAP;                             /* 右邻左端落在释放区间内（契约违反） */

    /* ── 融合边界（只并纯邻接）── */
    const bool merge_left = has_left && (left_it->base + left_it->len == base);
    const bool merge_right = (it != pos_.end()) && (it->base == base + len);
    uint64_t new_base = merge_left ? left_it->base : base;
    uint64_t new_end = merge_right ? (it->base + it->len) : (base + len);

    /* ── 先删后插 ──
     * 先删左邻再删右邻：erase 只失效被删迭代器，故删完左邻后 it 仍有效。
     * erase_both 内部是「从树先、主树后」，逐节点维持从 ⊆ 主。 */
    if (merge_left) erase_both(pos_, size_, left_it);
    if (merge_right) erase_both(pos_, size_, it);

    insert_both(pos_, size_, new_base, new_end - new_base);

    free_count_ += len;
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * free_intervals — 多段版 free_interval（消费调用方链）
 *
 * 契约：*runs == nullptr → RC_ERR_STATE（不动任何段）；逐段 pop_front
 * 取走节点后调 free_interval；遇错**已消费段不回滚**（释放天然不可逆），
 * 余段留在链上供调用方续查。全部处理完 → RC_OK（链已空）。
 * 单链节点生命周期归调用方；本函数只 drain 节点、不 delete 容器。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t mgr_slice_t::free_intervals(fs_run_list_t *runs) {
    if (runs == nullptr) return RC_ERR_STATE;
    if (!is_workable) return RC_ERR_STATE;

    while (!runs->empty()) {
        const fs_run_t run = runs->front();   /* 先取副本 */
        runs->pop_front();                    /* 再消费节点（消费即移除，不回滚） */
        const result_code_t rc = free_interval(run.base, run.len);
        if (rc != RC_OK) return rc;           /* 余段保留在链上；已消费段不回滚 */
    }
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * all_dump — 主树中序 = base 递增序，全量快照进调用方双链表
 *
 * 纯内存零 IO（IO 永远在锁外的纪律：调用方持锁取快照 → 解锁 → 落盘）。
 * *out_runs == nullptr → RC_ERR_STATE；尾插构建，不 clear/不 delete 容器
 * （容器生命周期归调用方）。片内全空 → 空链 + RC_OK。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t mgr_slice_t::all_dump(fs_run_dlist_t *out_runs) {
    if (out_runs == nullptr) return RC_ERR_STATE;
    if (!is_workable) return RC_ERR_STATE;

    for (const fs_interval_t &iv : pos_) {          /* fs_pos_less = base 升序 ⇒ 中序即递增 */
        out_runs->push_back(fs_run_t{iv.base, iv.len});
    }
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * soft_occupy — 锚定软占领（定点对·对偶 free_interval）
 *
 * 语义严格对照 bitmap_slices.cpp 的 soft_occupy，换树实现：
 *   *rest_bits 入 = 已满足量（链式记账）；real = len − *rest_bits 为本锚点
 *   目标上限；从全局 base 起贪心认领**连续空闲前缀**，claimed ≤ real；
 *   撞占用 / 上限即停；*rest_bits 出 = len − claimed（总缺口，恒等式与入值无关）。
 *   锚点即占用（claimed==0）→ RC_ERR_NO_BLOCK（rest 出 = len）。参数非法 → RC_ERR_STATE。
 *
 * 树实现要点（区别于位图的逐位贪心）：
 *   空闲区间极大 ⇒「base 起连续空闲前缀」=「包含 base 的那个空闲区间从 base 到
 *   其右端」。故只需一次 upper_bound 退格定位容器区间，claimed = min(real, 区间右端−base)。
 *   占领后按「缩 / 劈 / 删」改写该区间：
 *     - base 前有残余 → 保留左段 [iv.base, base)；
 *     - claimed 未吃到区间尾 → 保留右段 [base+claimed, iv.end)；
 *     两段都无 → 直接删。
 *   改写一律「先删后插」（std::set 元素 const，不原地改 key/len），双树同步。
 *   hint 只进不退：base+claimed > 当前 hint 才推进（锚定占领由政策驱动，
 *   回卷游标会打烂搜索分配的空间局部性）。
 *
 * 片尾夹断：claimed 同时受 slice_end = 起点+slice_bitscount_ 夹断（严格永不越片；
 *   空闲区间本就落在片内，此处为显式双保险）。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t mgr_slice_t::soft_occupy(uint64_t base, uint64_t len, uint64_t *rest_bits) {
    if (!is_workable || rest_bits == nullptr) return RC_ERR_STATE;
    if (*rest_bits > len) return RC_ERR_STATE;             /* 口径防御：已满足量不能超需求 */
    if (len == 0) return RC_OK;                            /* 无事可做（rest 不动） */
    const uint64_t real = len - *rest_bits;                /* 本锚点目标上限 */
    if (real == 0) return RC_OK;                           /* 已全满足 */
    if (base < in_global_base_idx) return RC_ERR_STATE;    /* 片首之下 */
    const uint64_t slice_end = in_global_base_idx + slice_bitscount_;
    if (base > slice_end) return RC_ERR_STATE;             /* 片尾之上（base==片尾 → 无可占领） */

    /* 定位「可能包含 base」的空闲区间：upper_bound(base) 退一格即为 base ≤ base' 的最大者。 */
    const fs_interval_t probe = make_pos_probe(base);
    auto it = pos_.upper_bound(probe);                     /* 第一个 base' > base */
    if (it == pos_.begin()) { *rest_bits = len; return RC_ERR_NO_BLOCK; }  /* base 在所有区间之前 */
    --it;
    if (base >= it->base + it->len) {                      /* 前驱到不了 base：base 处于占用位 */
        *rest_bits = len;
        return RC_ERR_NO_BLOCK;
    }

    /* base 落在 [it->base, it->base+it->len) 内 ⇒ 连续空闲前缀可达区间右端。 */
    const uint64_t iv_end = it->base + it->len;
    uint64_t claimed = iv_end - base;                      /* 可用前缀长（≥1） */
    if (claimed > real) claimed = real;                    /* 额度上限夹断 */
    if (base + claimed > slice_end) claimed = slice_end - base;  /* 片尾夹断（严格永不越片） */

    const uint64_t left_len  = base - it->base;            /* 残余左段 [iv.base, base) */
    const uint64_t left_base = it->base;
    const uint64_t right_base = base + claimed;            /* 残余右段 [base+claimed, iv_end) */
    const uint64_t right_len = iv_end - right_base;

    /* 先删原区间（从树先、主树后），再按残余插回（主树先、从树后）。 */
    erase_both(pos_, size_, it);
    if (left_len > 0)  insert_both(pos_, size_, left_base, left_len);
    if (right_len > 0) insert_both(pos_, size_, right_base, right_len);

    free_count_ -= claimed;
    if (base + claimed > last_finish_hint_) last_finish_hint_ = base + claimed;  /* 只进不退 */
    *rest_bits = len - claimed;
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * alloc_hard — 硬分配：best-fit 选区间 + 头部下挖，不部分分配
 *
 * 选区间（best-fit）：从树 size_ 按 (len 主, base 辅) 复合序升序；用探针
 *   (len=bitscount, base=0) 做 lower_bound，即「最小的一批 len ≥ bitscount」
 *   的头。选尺寸最匹配者 = 把剩余碎片降到最小（“改变的字段最少”的动机）。
 *   对齐可能让首个候选盛不下（区间里找不到满足 2^align_log2 对齐的起点），
 *   故沿 len 非降序继续试，**首个可行者**即 best-fit（不破坏 best-fit 语义：
 *   第一个能容纳的、len 最小的区间）。
 *
 * 下挖位置（**从尾部下挖**）：分配段起点 s = (区间尾 − bitscount) 向下取到
 *   2^align_log2 边界（align_log2==0 时 s 恰为区间尾−bitscount）——即尽量贴尾。
 *   于是余段 = [区间头, s)，**base 不变、只 len 缩短**（改动字段最少）；只有
 *   对齐逼得贴不到尾时，才多出一段尾余 [s+bitscount, 区间尾)。
 *
 * 记账与一致性：挖取 [s, s+bitscount)；先 erase_both 原区间、再把残余
 *   [iv.base, s) 与 [s+bitscount, iv.end) 逐段 insert_both（先删后插 + 从⊆主）；
 *   free_count_ -= bitscount；hint 只进不退。
 *
 * 坐标：全局位号。范围：区间必在片内 ⇒ 分配段必在片内（无需再查上界）。
 * 失败：整片找不到能容纳对齐段且 len≥bitscount 的区间 → RC_ERR_NO_BLOCK
 *   （原子：不部分分配）。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t mgr_slice_t::alloc_hard(uint8_t align_log2, uint64_t bitscount,
                                      uint64_t &out_base_idx) {
    if (!is_workable) return RC_ERR_STATE;
    if (bitscount == 0) return RC_ERR_STATE;
    if (align_log2 > 63) return RC_ERR_STATE;              /* 对齐参数非法 */

    const uint64_t align_mask = (align_log2 == 0) ? 0ULL : ((1ULL << align_log2) - 1);

    fs_interval_t probe{};                                 /* 从树探针：(len, base) */
    probe.base = 0;
    probe.len  = bitscount;

    for (auto it = size_.lower_bound(probe); it != size_.end(); ++it) {
        const uint64_t iv_base = it->base;
        const uint64_t iv_len  = it->len;
        if (iv_len < bitscount) break;                     /* 防御：升序，不应发生 */
        const uint64_t iv_end = iv_base + iv_len;

        const uint64_t s = (iv_end - bitscount) & ~align_mask;   /* 尾部下挖起点（尽量贴尾 + 对齐） */
        if (s < iv_base) continue;                         /* 对齐后落到区间头之下 → 本区间容不下 */

        /* 定位主树同元素（base 唯一）——erase_both 需要主树迭代器。 */
        auto pit = pos_.lower_bound(make_pos_probe(iv_base));
        erase_both(pos_, size_, pit);                      /* 先删（从树先、主树后） */

        const uint64_t left_len   = s - iv_base;           /* 余段 [iv.base, s)：base 不变、只 len 缩短 */
        const uint64_t right_base = s + bitscount;         /* 尾余（仅对齐贴不到尾时存在） */
        const uint64_t right_len  = iv_end - right_base;
        if (left_len  > 0) insert_both(pos_, size_, iv_base, left_len);   /* 再插（主树先、从树后） */
        if (right_len > 0) insert_both(pos_, size_, right_base, right_len);

        free_count_ -= bitscount;
        if (right_base > last_finish_hint_) last_finish_hint_ = right_base;  /* 只进不退 */
        out_base_idx = s;
        return RC_OK;
    }
    return RC_ERR_NO_BLOCK;                                /* 满片：原子失败，不部分分配 */
}

/* ═══════════════════════════════════════════════════════════════
 * shutdown — enable_slice 的对位操作（true→false 并拆树清账）
 *
 * 契约（头注释）：必须从 is_workable==true 转 false 才成功；初始化态
 *   （is_workable==false）调用 → RC_ERR_STATE。
 * 动作：清空双树（std::set::clear + boost clear 均释放节点）、free_count_ /
 *   hint 归零、内部化字段（起点/位数）也归零——与 enable 的写入严格对位，
 *   使对象回到「全新未使能」态，可再次 enable。
 * 锁/IO：同其它接口，调用方持 slice_lock；片内零 IO。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t mgr_slice_t::shutdown() {
    if (!is_workable) return RC_ERR_STATE;                 /* 初始化态：无可 shutdown */
    clear_both(pos_, size_);                               /* 先摘从树钩子再析构主树元素（bug#2） */
    free_count_ = 0;
    last_finish_hint_ = 0;
    in_global_base_idx = 0;                                /* 对位：清内部化字段 */
    slice_bitscount_   = 0;
    is_workable = false;
    return RC_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * soft_alloc — 软分配（无锚·**碎片优先**）：从从树小碎片端起逐段消费
 *
 * 策略（用户裁 2026-10-03）：碎片优先。size_ 按 (len,base) 升序 ⇒ begin()
 *   即当前最小碎片。只从小端消费：大块（尾巴）不碰，留给 alloc_hard。
 *   整段优先（t == 段长 时整段吃），只有最后一段不足时才劈（挖尾，余段 base 不变）。
 *   不做 opcode / 双函数——无第二调用者前不引入策略旋钮。
 *
 * 终止性：每轮要么 carve（remain 严格下降，carve 次数 ≤ 碎片数 + 1），要么
 *   `++it` 跳过不可用段（有限），故必停。carve 后 `it = begin()` 重取最小——
 *   本次 carve 的余段（更小）可能改写顺序，重取才能继续碎片优先。
 *
 * 对齐定点（与 alloc_hard 同源）：尽量贴尾 + 2^align_log2 对齐；贴不到头时
 *   退化为「自区间头向上对齐」的兜底；两者都落不下则跳过本段。
 *
 * 交付语义：**允许部分交付**（best-effort 本分）。all_satisfied = 本片本次是否
 *   已达 bitscount（Σ len ≥ bitscount）——**分片级**判决；跨分片全局判决归路由层。
 *   一位未扣 → RC_ERR_NO_BLOCK（此时 all_satisfied=false）。
 * ═══════════════════════════════════════════════════════════════ */
result_code_t mgr_slice_t::soft_alloc(uint8_t align_log2, uint64_t bitscount,
                                      fs_run_list_t *out_runs, bool &all_satisfied) {
    if (!is_workable) return RC_ERR_STATE;
    if (out_runs == nullptr) return RC_ERR_STATE;
    if (align_log2 > 63) return RC_ERR_STATE;
    all_satisfied = false;
    if (bitscount == 0) return RC_ERR_STATE;               /* 同 alloc_hard：0 请求视为非法 */

    const uint64_t align_mask = (align_log2 == 0) ? 0ULL : ((1ULL << align_log2) - 1);
    uint64_t remain = bitscount;
    uint64_t taken_total = 0;

    auto it = size_.begin();                               /* 小端起点 = 最小碎片 */
    while (it != size_.end() && remain > 0) {
        const uint64_t iv_base = it->base, iv_len = it->len, iv_end = iv_base + iv_len;

        uint64_t t = (iv_len < remain) ? iv_len : remain;  /* 整段优先，最多取 remain */
        uint64_t s = (iv_end - t) & ~align_mask;           /* 尽量贴尾 + 对齐 */
        if (s < iv_base) {                                 /* 贴尾对齐落到区间头之下 */
            s = (iv_base + align_mask) & ~align_mask;      /* 头对齐兜底 */
            if (s >= iv_end) { ++it; continue; }           /* 本段对齐后无可占 → 跳过 */
            const uint64_t avail = iv_end - s;
            t = (avail < remain) ? avail : remain;
        }

        auto pit = pos_.lower_bound(make_pos_probe(iv_base));   /* base 唯一，定位主树同元素 */
        erase_both(pos_, size_, pit);                      /* 先删（从树先、主树后） */
        const uint64_t left_len   = s - iv_base;           /* 余段 [iv.base, s)：base 不变 */
        const uint64_t right_base = s + t;                 /* 尾余（贴尾时为空） */
        const uint64_t right_len  = iv_end - right_base;
        if (left_len  > 0) insert_both(pos_, size_, iv_base, left_len);   /* 再插（主树先、从树后） */
        if (right_len > 0) insert_both(pos_, size_, right_base, right_len);

        out_runs->push_front(fs_run_t{s, t});              /* 前插构建（容器归调用方） */
        free_count_ -= t;
        taken_total += t;
        remain -= t;
        if (right_base > last_finish_hint_) last_finish_hint_ = right_base;  /* 只进不退 */

        it = size_.begin();                                /* 余段可能更小 → 重取最小，继续碎片优先 */
    }

    if (taken_total == 0) return RC_ERR_NO_BLOCK;          /* 一位未扣：原子空手 */
    all_satisfied = (remain == 0);                         /* 分片级是否已满足 */
    return RC_OK;                                          /* 允许部分交付 */
}

/* ═══════════════════════════════════════════════════════════════════
 * inode_data_plane_t — 全局 inode 数据面实现（v2.0）
 *
 * 复用本文件顶部的匿名 namespace 轮子（insert_both / erase_both / clear_both /
 * make_pos_probe）维护 fr 主从树；extent 树是普通 std::set（key=lblk）。
 *
 * 锁序（见头注，硬）：fr_lock_（空间） > extent_lock_（翻译）；fr_lock_ > block 片锁。
 *   · locate           : extent 读
 *   · alloc            : fr 写；用尽 → grow_locked（fr 写内再 extent 写 + block 片锁）
 *   · free / test / 几何: fr 读或写
 * ═══════════════════════════════════════════════════════════════════ */

inode_data_plane_t::~inode_data_plane_t() {
    /* 主从树析构纪律：先摘从树钩子（size_），再析构主树元素（pos_）。 */
    fr_size_.clear();
    fr_pos_.clear();
    extents_.clear();
}

result_code_t inode_data_plane_t::init(const inode_data_plane_cfg_t &cfg, fs_allocator_t *owner) {
    if (owner == nullptr) return RC_ERR_STATE;
    if (cfg.pieces == 0) return RC_ERR_STATE;
    if (cfg.extent_count == 0 || cfg.extents == nullptr) return RC_ERR_STATE;

    owner_ = owner;
    grow_batch_ = cfg.grow_batch_pieces ? cfg.grow_batch_pieces : 1;
    pieces_ = 0;
    fr_free_ = 0;

    /* ── 翻译面：校验 extent 稠密无洞无重叠 + 8K 对齐 + Σ ──
     * 不变量（disk_layout_v2.0 §4.5）：lblk 偶、phys_base 偶、length 偶>0，
     * 排序后 [0] 起于 0 且逐条紧邻，Σlength == pieces×2。任一不符 → 挂载失败。 */
    {
        uint64_t expect_lblk = 0;
        try {
            for (uint64_t i = 0; i < cfg.extent_count; i++) {
                const fs_inode_extent_t e = cfg.extents[i];
                if (e.length == 0 || (e.length & 1ULL) || (e.lblk & 1ULL) || (e.phys_base & 1ULL))
                    return RC_ERR_STATE;                       /* 尺寸/对齐非法（2 块对齐铁律） */
                if (e.lblk != expect_lblk) return RC_ERR_STATE;/* 洞 / 重叠 / 未排序 */
                extents_.insert(e);
                expect_lblk = e.lblk + e.length;
            }
        } catch (const std::bad_alloc &) {
            extents_.clear();
            return RC_ERR_STATE;
        }
        if (expect_lblk != cfg.pieces * 2ULL) { extents_.clear(); return RC_ERR_STATE; }
    }
    pieces_ = cfg.pieces;

    /* ── 空闲面：范围校验（落 [0,pieces×32)、不含 idx0、互不重叠）+ 主从插入 ──
     * insert_both 只在「邻接」时融合；重叠是契约违反 → 报错挂载失败（绝不静默改写）。 */
    const uint64_t total_inodes = pieces_ * 32ULL;
    try {
        for (uint64_t i = 0; i < cfg.fr_count; i++) {
            const inode_dataplane_coordinate_free_range_t f = cfg.fr[i];
            if (f.len == 0) continue;
            if (f.base == 0) goto fr_fail;                     /* idx0 归 root，永不可空闲 */
            if (f.base >= total_inodes || f.len > total_inodes - f.base) goto fr_fail;  /* 越界 */
            {
                const fs_interval_t probe = make_pos_probe(f.base);
                auto it = fr_pos_.lower_bound(probe);
                if (it != fr_pos_.end() && it->base < f.base + f.len) goto fr_fail;      /* 与后继重叠 */
                if (it != fr_pos_.begin()) {
                    auto p = std::prev(it);
                    if (p->base + p->len > f.base) goto fr_fail;                          /* 与前驱重叠 */
                }
            }
            insert_both(fr_pos_, fr_size_, f.base, f.len);
            fr_free_ += f.len;
        }
    } catch (const std::bad_alloc &) {
        goto fr_fail;
    }
    return RC_OK;

fr_fail:
    fr_size_.clear();
    fr_pos_.clear();
    return RC_ERR_STATE;
}

result_code_t inode_data_plane_t::locate(uint64_t inum, inode_location_t *out) const {
    if (out == nullptr) return RC_ERR_STATE;
    std::shared_lock<std::shared_mutex> lk(extent_lock_);
    const uint64_t lblk = inum >> 4;                       /* 逻辑 4K 块坐标（映射树坐标） */
    fs_inode_extent_t probe{};
    probe.lblk = lblk;
    auto it = extents_.upper_bound(probe);                 /* 第一个 lblk > target */
    if (it == extents_.begin()) return RC_ERR_NOT_FOUND;
    --it;
    if (lblk >= it->lblk + it->length) return RC_ERR_NOT_FOUND;
    /* 8K 片基 = 覆盖该 lblk 的偶块起点；片内偏移 = (inum & 31)×256。 */
    out->blkbase = it->phys_base + ((lblk - it->lblk) & ~1ULL);
    out->inblk_offset = (uint16_t)((inum & 31ULL) * 256ULL);
    return RC_OK;
}

/* alloc_locked：fr best-fit 取一个 inode（须持 fr_lock_ 写锁）。
 * need=1 ⇒ 最小 len 区间即最优；取其 base（碎片复用 + 计数局部性）。 */
result_code_t inode_data_plane_t::alloc_locked(uint64_t &out_inum) {
    if (fr_size_.empty()) return RC_ERR_NO_BLOCK;
    auto it = fr_size_.begin();
    const uint64_t b = it->base, l = it->len;
    auto pit = fr_pos_.lower_bound(make_pos_probe(b));
    if (pit == fr_pos_.end() || pit->base != b || pit->len != l) return RC_ERR_STATE;  /* 双树失配：内部错误 */
    erase_both(fr_pos_, fr_size_, pit);
    if (l > 1) insert_both(fr_pos_, fr_size_, b + 1, l - 1);
    fr_free_ -= 1;
    out_inum = b;
    return RC_OK;
}

/* grow_locked：fr 写锁内增片。先 block 硬分配（2k 块、8K 对齐，k 逐级折半降至 1），
 * 再**先翻译后 fr**：注入 extent（或与物理尾邻接时扩尾）→ 新片 32k 个 inode 入 fr。
 * 失败回滚：块已取但 extent 注入抛错 → block_free 退片。 */
result_code_t inode_data_plane_t::grow_locked() {
    uint32_t k = grow_batch_ ? grow_batch_ : 1;
    uint64_t blk = PTR_INVALID;
    for (;;) {
        const uint64_t b = owner_->block_alloc_hard((uint64_t)k * 2ULL, 1);  /* 2k 块，8K 对齐 */
        if (b != PTR_INVALID) { blk = b; break; }
        if (k <= 1) break;
        k >>= 1;
        if (k == 0) k = 1;
    }
    if (blk == PTR_INVALID) return RC_ERR_NO_BLOCK;

    const uint64_t new_lblk = pieces_ * 2ULL;   /* 逻辑尾（稠密无洞） */
    try {
        std::unique_lock<std::shared_mutex> elk(extent_lock_);   /* 锁序：fr → ext */
        bool merged = false;
        if (!extents_.empty()) {
            auto last = std::prev(extents_.end());
            if (last->lblk + last->length == new_lblk &&
                last->phys_base + last->length == blk) {         /* 物理尾邻接 → 扩尾（合并） */
                fs_inode_extent_t ne = *last;
                ne.length += (uint64_t)k * 2ULL;
                extents_.erase(last);
                extents_.insert(ne);
                merged = true;
            }
        }
        if (!merged) {
            fs_inode_extent_t ne{};
            ne.lblk      = new_lblk;
            ne.phys_base = blk;
            ne.length    = (uint64_t)k * 2ULL;
            extents_.insert(ne);
        }
    } catch (const std::bad_alloc &) {
        owner_->block_free(blk, (uint64_t)k * 2ULL);             /* 回滚：退片 */
        return RC_ERR_STATE;
    }

    const uint64_t old_pieces = pieces_;
    pieces_ += k;
    insert_both(fr_pos_, fr_size_, old_pieces * 32ULL, (uint64_t)k * 32ULL);
    fr_free_ += (uint64_t)k * 32ULL;
    return RC_OK;
}

result_code_t inode_data_plane_t::alloc(uint64_t &out_inum) {
    std::unique_lock<std::shared_mutex> lk(fr_lock_);
    result_code_t rc = alloc_locked(out_inum);
    if (rc == RC_OK) return rc;
    if (rc != RC_ERR_NO_BLOCK) return rc;
    rc = grow_locked();                        /* 用尽：fr 写内 grow（ext 写 + block 片锁） */
    if (rc != RC_OK) return rc;
    return alloc_locked(out_inum);
}

bool inode_data_plane_t::fr_contains(uint64_t inum) const {
    const fs_interval_t probe = make_pos_probe(inum);
    auto it = fr_pos_.upper_bound(probe);      /* 第一个 base > inum */
    if (it == fr_pos_.begin()) return false;
    --it;
    return inum < it->base + it->len;
}

void inode_data_plane_t::free_inode(uint64_t inum) {
    if (inum == 0) return;                     /* root 永不释放 */
    std::unique_lock<std::shared_mutex> lk(fr_lock_);
    if (inum >= pieces_ * 32ULL) return;       /* 越界忽略 */
    if (fr_contains(inum)) return;             /* 幂等：重复释放忽略 */
    insert_both(fr_pos_, fr_size_, inum, 1);   /* 邻接自动融合 */
    fr_free_ += 1;
}

bool inode_data_plane_t::test_inode(uint64_t inum) const {
    std::shared_lock<std::shared_mutex> lk(fr_lock_);
    if (inum >= pieces_ * 32ULL) return false;
    return !fr_contains(inum);                 /* 在空间内且不在 fr ⇒ 已分配 */
}

uint64_t inode_data_plane_t::pieces() const {
    std::shared_lock<std::shared_mutex> lk(fr_lock_);
    return pieces_;
}

uint64_t inode_data_plane_t::free_total() const {
    std::shared_lock<std::shared_mutex> lk(fr_lock_);
    return fr_free_;
}

uint64_t inode_data_plane_t::extent_count() const {
    std::shared_lock<std::shared_mutex> lk(extent_lock_);
    return extents_.size();
}

result_code_t inode_data_plane_t::dump_fr(std::list<inode_dataplane_coordinate_free_range_t> *out) const {
    if (out == nullptr) return RC_ERR_STATE;
    std::shared_lock<std::shared_mutex> lk(fr_lock_);
    for (const fs_interval_t &iv : fr_pos_)
        out->push_back(inode_dataplane_coordinate_free_range_t{iv.base, iv.len});
    return RC_OK;
}

result_code_t inode_data_plane_t::dump_extents(std::list<fs_inode_extent_t> *out) const {
    if (out == nullptr) return RC_ERR_STATE;
    std::shared_lock<std::shared_mutex> lk(extent_lock_);
    for (const fs_inode_extent_t &e : extents_) out->push_back(e);
    return RC_OK;
}
