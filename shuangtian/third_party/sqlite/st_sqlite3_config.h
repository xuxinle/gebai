/*
** 霜天（Shuangtian）内置 SQLite 的编译期开关——**不改上游一行代码**。
**
** 上游 `sqlite3.c` 是 amalgamation 单文件，其全部功能由一组 `SQLITE_*` 宏决定。
** 本文件把这些宏集中在一处，由 `st.pkg` 以 `-include` 强制前置到 sqlite3.c 的编译
** （见 st.pkg 的 `c_flags`），因此：
**   - 上游文件保持**原样**（可逐字节校验 SHA-256，见 third_party/SOURCES.md），
**     要加/减能力只动本文件，diff 一眼可见；
**   - 开关是**编译期**的事实：SQLite 把它们烧进库的语义里，改一个宏必须重编 sqlite3.c
**     （stpm 把编译标志纳入增量判据，见 src/pkg/build.cpp 的 compile_fingerprint）。
**
** 配置取舍的原则：**默认零配置可用**（不需要任何应用侧初始化），但**不替应用做危险的事**。
**
** 逐项说明（未列出的宏 = 取上游默认值）：
**
**   SQLITE_THREADSAFE=1        serialized：一个连接可跨线程使用（库内自带互斥）。
**                              默认值本就是 1，这里显式写出来，是因为它是**对外承诺**——
**                              st::ext::Database 的线程语义按它写（见 include/st/ext/database.hpp）。
**   SQLITE_DEFAULT_MEMSTATUS=0 关掉运行时内存统计：每次分配少一次原子计数，
**                              代价是 sqlite3_memory_used() 之类不再可用（我们不用它）。
**   SQLITE_DQS=0               禁止双引号当字符串字面量（`"x"` 只当标识符）：
**                              SQL 里写错的引号会当场报错，而不是静默变成字符串——
**                              "查询没报错但结果不对"是数据层最难查的一类问题。
**   SQLITE_DEFAULT_FOREIGN_KEYS=1 外键约束默认打开：现代应用里静默忽略外键是更大的坑。
**   SQLITE_ENABLE_API_ARMOR 1  给 API 入口加参数合法性检查：传空指针/错序号时**返回错误码**
**                              而不是段错误。代价是每个 API 调用多几次判断，换"错误可诊断"。
**   SQLITE_ENABLE_COLUMN_METADATA 1
**                              提供 sqlite3_column_table_name() 等来源信息（Query→表名溯源）。
**   SQLITE_ENABLE_DBSTAT_VTAB 1 `dbstat` 虚表：能查"每个表/索引占了多少页"——
**                              应用要显示"哪个表吃磁盘"时不必自己扫文件。
**   SQLITE_ENABLE_FTS5 1       全文检索（FTS5）：霜天的检索场景少不了它。
**   SQLITE_ENABLE_MATH_FUNCTIONS 1
**                              sin/cos/ln/pow 等 SQL 标量函数（Linux 上由 `-lm` 提供）。
**   SQLITE_ENABLE_RTREE 1      空间索引 R-tree（按矩形范围查询）。
**   SQLITE_ENABLE_STAT4 1      更准的查询规划统计（analyze 后优化器选得更对）。
**   SQLITE_OMIT_DEPRECATED 1   剔除已废弃的老 API（应用不该用它们；省体积、少误用面）。
**   SQLITE_OMIT_LOAD_EXTENSION 1
**                              **不提供**运行时加载原生 DLL/.so 的能力（sqlite3_load_extension()、
**                              load_extension() SQL 函数一并消失）。这与框架对脚本层的姿态一致：
**                              可执行代码的入口必须显式设计，不能"因为方便就默认打开"。
**   SQLITE_OMIT_SHARED_CACHE 1 剔除 shared-cache 模式（多连接共享一个页缓存）：它需要线程级
**                              锁表，收益有限而失败模式隐蔽；并发读用 WAL + 多连接即可。
**
** 刻意**不**打开的（不是遗漏）：
**   - SQLITE_ENABLE_SESSION / PREUPDATE_HOOK：变更集捕获，本框架无此需求；
**   - SQLITE_ENABLE_UNLOCK_NOTIFY：需要应用侧回调编排，暂不引入；
**   - SQLITE_DEBUG / SQLITE_ENABLE_MEMORY_MANAGEMENT：调试与显式内存管理档，生产不需要；
**   - SQLITE_ENABLE_JSON1：**3.38 起 JSON 函数已默认编入**，再定义只是历史写法；
**   - SQLITE_OMIT_AUTOINIT / SQLITE_OMIT_WAL：前者要求调用方显式初始化（易漏），
**     后者会砍掉并发读写能力——两者都会让"默认可用"这条原则落空。
*/

#ifndef ST_SQLITE3_CONFIG_H
#define ST_SQLITE3_CONFIG_H

/* ——线程与内存—— */
#define SQLITE_THREADSAFE 1
#define SQLITE_DEFAULT_MEMSTATUS 0

/* ——SQL 语义（更严格、更安全）—— */
#define SQLITE_DQS 0
#define SQLITE_DEFAULT_FOREIGN_KEYS 1

/* ——健壮性—— */
#define SQLITE_ENABLE_API_ARMOR 1

/* ——能力开关—— */
#define SQLITE_ENABLE_COLUMN_METADATA 1
#define SQLITE_ENABLE_DBSTAT_VTAB 1
#define SQLITE_ENABLE_FTS5 1
#define SQLITE_ENABLE_MATH_FUNCTIONS 1
#define SQLITE_ENABLE_RTREE 1
#define SQLITE_ENABLE_STAT4 1

/* ——裁掉不用的—— */
#define SQLITE_OMIT_DEPRECATED 1
#define SQLITE_OMIT_LOAD_EXTENSION 1
#define SQLITE_OMIT_SHARED_CACHE 1

#endif /* ST_SQLITE3_CONFIG_H */
