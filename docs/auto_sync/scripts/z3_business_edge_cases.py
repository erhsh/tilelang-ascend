#!/usr/bin/env python3
"""
测试标准 CanProve 在 Ascend 业务场景中的极限
目标：找到 CanProve 解决不了但 Z3 能解决的真实业务问题

测试策略：构造越来越复杂的约束，逐步逼近 CanProve 的极限
"""

import sys
sys.path.insert(0, '/mnt/workspace/developer/workspace/git/github/erhsh/tilelang-ascend')

import tilelang
import tvm.arith as arith
import tvm.tirx as tir

def section(title):
    print(f"\n{'='*70}")
    print(f"  {title}")
    print(f"{'='*70}")

def test(name, result, expected=None):
    """打印测试结果。expected=None 表示只观察，expected=True/False 表示预期"""
    status = "✓" if result else "✗"
    info = ""
    if expected is not None:
        if result == expected:
            info = " ← 符合预期"
        else:
            info = " ← ⚠️ 意外!"
    print(f"[{status}] {name}: {result}{info}")
    return result

analyzer = arith.Analyzer()

# ============================================================================
section("类别 1: 多层 if 嵌套约束（业务：PipelineStage 条件分支）")
# 在 Ascend 中，kernel 常有 pipeline stage 判断：
#   if (stage == PIPE_MTE2) { ... }
#   if (stage == PIPE_M) { ... }
# 等价于深层嵌套的 if/else
# ============================================================================

i = tir.Var('i', 'int32')
j = tir.Var('j', 'int32')
k = tir.Var('k', 'int32')

# 深层嵌套：3 个条件同时成立
with analyzer.constraint_scope(tir.And(i >= 0, i < 128)):
    with analyzer.constraint_scope(tir.And(j >= 0, j < 128)):
        with analyzer.constraint_scope(tir.And(k >= 0, k < 128)):
            with analyzer.constraint_scope(tir.EQ(i, j + 2)):
                test("i == j + 2 下: i != j",
                     analyzer.can_prove(tir.NE(i, j)),
                     expected=True)
                test("i == j + 2 下: i != k（无约束关系）",
                     analyzer.can_prove(tir.NE(i, k)),
                     expected=False)  # 无约束，可能相等

# ============================================================================
section("类别 2: 模算术嵌套（业务：Double Buffer / 模缓冲）")
# Ascend DMA 常用双缓冲：buf[i % 2]、buf[i % 4]
# ============================================================================

i = tir.Var('i', 'int32')
j = tir.Var('j', 'int32')

with analyzer.constraint_scope(tir.And(i >= 0, i < N)) if False else analyzer:
    pass
N_var = tir.Var('N', 'int32')

with analyzer.constraint_scope(tir.And(i >= 0, i < 1024)):
    # 双缓冲：当前 i%2 vs 下一个 (i+1)%2
    test("i%2 != (i+1)%2",
         analyzer.can_prove(tir.NE(tir.floormod(i, 2), tir.floormod(i+1, 2))),
         expected=True)
    
    # 三缓冲
    test("i%3 != (i+1)%3",
         analyzer.can_prove(tir.NE(tir.floormod(i, 3), tir.floormod(i+1, 3))),
         expected=True)
    
    # 双缓冲跨两步：i%2 vs (i+2)%2（应该相等！）
    test("i%2 == (i+2)%2",
         analyzer.can_prove(tir.EQ(tir.floormod(i, 2), tir.floormod(i+2, 2))),
         expected=True)
    
    # 复杂：模下加法
    # (a + b) % n == ((a%n) + (b%n)) % n
    test("(i+j)%4 的范围 [0,3]",
         analyzer.can_prove(tir.And(tir.GE(tir.floormod(i+j, 4), 0),
                                     tir.LE(tir.floormod(i+j, 4), 3))),
         expected=True)

# ============================================================================
section("类别 3: 非线性算术（业务：GEMM 的 M*N 尺寸约束）")
# Flash Attention / GEMM 常有 M、N、K 符号常量
# ============================================================================

M = tir.Var('M', 'int32')
N = tir.Var('N', 'int32')
K = tir.Var('K', 'int32')

with analyzer.constraint_scope(tir.And(M >= 16, M <= 256)):
    with analyzer.constraint_scope(tir.And(N >= 16, N <= 256)):
        with analyzer.constraint_scope(tir.And(K >= 16, K <= 256)):
            # 非线性乘积
            test("M*N*16 < 256*256*16 (=1048576)",
                 analyzer.can_prove(tir.LT(M*N*16, 256*256*16)),
                 expected=True)
            
            # 非线性但边界紧张
            test("M*N <= 65536 (=256*256)",
                 analyzer.can_prove(tir.LE(M*N, 65536)),
                 expected=True)
            
            test("M*N*K < 2^24 (=16777216)",
                 analyzer.can_prove(tir.LT(M*N*K, 16777216)),
                 expected=True)
            
            # GEMM 常用：M*K + N*K <= (M+N)*K
            # 这是恒等式，应该可以 trivially 证明
            test("M*K + N*K == (M+N)*K（恒等式）",
                 analyzer.can_prove(tir.EQ(M*K + N*K, (M+N)*K)),
                 expected=True)

# ============================================================================
section("类别 4: FloorDiv + FloorMod 组合（业务：Tiling 索引）")
# Kernel tiling 常见：offset = (i//tile)*tile + i%tile
# ============================================================================

i = tir.Var('i', 'int32')
tile = tir.Var('tile', 'int32')

with analyzer.constraint_scope(tir.And(i >= 0, i < 1024)):
    with analyzer.constraint_scope(tir.EQ(tile, 64)):
        # 经典恒等式：i == (i // tile) * tile + (i % tile)
        lhs = i
        rhs = tir.floordiv(i, tile) * tile + tir.floormod(i, tile)
        test("i == (i//64)*64 + i%64（取模恒等式）",
             analyzer.can_prove(tir.EQ(lhs, rhs)),
             expected=True)
        
        # 范围推断：i // 64 在 [0, 15]
        test("i//64 在 [0, 15]",
             analyzer.can_prove(tir.And(tir.GE(tir.floordiv(i, tile), 0),
                                         tir.LE(tir.floordiv(i, tile), 15))),
             expected=True)

        # 跨 tile 边界：i=63 在 tile 0, i=64 在 tile 1
        # 证明：相邻 i 的 i//64 差 0 或 1
        test("floor_div(i+1, 64) - floor_div(i, 64) 在 [0, 1]",
             analyzer.can_prove(tir.And(
                 tir.GE(tir.floordiv(i+1, tile) - tir.floordiv(i, tile), 0),
                 tir.LE(tir.floordiv(i+1, tile) - tir.floordiv(i, tile), 1)
             )),
             expected=True)

# ============================================================================
section("类别 5: 约束等价性（GPU 版本的真正核心）")
# GPU 版本 FindConflict 的核心：证明两组约束集合 ConstrA 和 ConstrB 等价
# 即 ConstrA ⟺ ConstrB
# 用于判断\"同一组线程同时执行了两次访问\"
# ============================================================================

tx = tir.Var('tx', 'int32')

# 简单等价：A = B
A_conj = tir.And(tx >= 0, tx < 32)
B_conj = tir.And(tx >= 0, tx < 32)
with analyzer.constraint_scope(tir.And(tx >= 0, tx < 64)):
    a_implies_b = analyzer.can_prove(tir.Or(tir.Not(A_conj), B_conj))
    b_implies_a = analyzer.can_prove(tir.Or(tir.Not(B_conj), A_conj))
    test("A==B 的等价性（两个相同约束）",
         a_implies_b and b_implies_a,
         expected=True)

# 不等价：A 范围 [0,32)，B 范围 [0,16)
A_conj = tir.And(tx >= 0, tx < 32)
B_conj = tir.And(tx >= 0, tx < 16)
with analyzer.constraint_scope(tir.And(tx >= 0, tx < 64)):
    a_implies_b = analyzer.can_prove(tir.Or(tir.Not(A_conj), B_conj))  # False: e.g. tx=20
    b_implies_a = analyzer.can_prove(tir.Or(tir.Not(B_conj), A_conj))  # True: [0,16) ⊂ [0,32)
    test("[0,32) 与 [0,16) 等价性",
         a_implies_b and b_implies_a,
         expected=False)

# 复杂等价：多层约束
# A: tx in [0,64) AND ty in [0,64) AND tx%8 == 0
# B: tx in [0,64) AND ty in [0,64) AND (tx+8)%8 == 0
# (tx%8 == 0) 等价于 ((tx+8)%8 == 0)  ← 这个恒成立！
ty = tir.Var('ty', 'int32')
with analyzer.constraint_scope(tir.And(
    tir.And(tx >= 0, tx < 64),
    tir.And(ty >= 0, ty < 64)
)):
    # 证明 A1 ⟹ B1
    # A1: tx%8 == 0
    # B1: (tx+8)%8 == 0  ← 即 (tx%8 + 8%8)%8 = tx%8 == 0
    with analyzer.constraint_scope(tir.EQ(tir.floormod(tx, 8), 0)):
        test("tx%8==0 ⟹ (tx+8)%8==0",
             analyzer.can_prove(tir.EQ(tir.floormod(tx+8, 8), 0)),
             expected=True)
    
    # 反向
    with analyzer.constraint_scope(tir.EQ(tir.floormod(tx+8, 8), 0)):
        test("(tx+8)%8==0 ⟹ tx%8==0",
             analyzer.can_prove(tir.EQ(tir.floormod(tx, 8), 0)),
             expected=True)

# ============================================================================
section("类别 6: 存在性证明（Z3 的强项）")
# 某些问题：是否存在 i 使得 f(i) 满足某条件？
# Standard CanProve 只能证明 forall，不能证明 exists
# ============================================================================

i = tir.Var('i', 'int32')

# 存在性问题：i in [0, 100) 中，是否有 i%7 == 0？
# 这是 exists 问题，standard CanProve 不直接处理
# 但可以用 forall 间接：NOT forall i, i%7 != 0
with analyzer.constraint_scope(tir.And(i >= 0, i < 100)):
    # 标准：证明 forall i, i%7 != 0（这应该是 False，因为存在 i=0,7,14...）
    test("forall i in [0,100): i%7 != 0（应为 False）",
         analyzer.can_prove(tir.NE(tir.floormod(i, 7), 0)),
         expected=False)

# ============================================================================
section("类别 7: 业务真实场景 - Ascend Flash Attention Tiling")
# Tiling 配置:
#   BlockTile: [128, 128]
#   WarpTile: [64, 64]
#   kBlock = i // 2  (外层 tile 索引)
#   kWarp = i % 2   (内层 warp 索引)
# 证明：相邻 i 与 i+1 的 kWarp 不同
# ============================================================================

i = tir.Var('i', 'int32')

with analyzer.constraint_scope(tir.And(i >= 0, i < 8)):  # 8 个 warp
    # 双 warp 配置
    test("Flash Attn tiling: i%2 != (i+1)%2",
         analyzer.can_prove(tir.NE(tir.floormod(i, 2),
                                    tir.floormod(i+1, 2))),
         expected=True)
    
    # 四 warp 配置
    test("Flash Attn tiling: i%4 != (i+1)%4",
         analyzer.can_prove(tir.NE(tir.floormod(i, 4),
                                    tir.floormod(i+1, 4))),
         expected=True)
    
    # 更复杂：i//2 在相邻 i 之间
    test("Flash Attn: i//2 与 (i+1)//2 差 0 或 1",
         analyzer.can_prove(tir.And(
             tir.GE(tir.floordiv(i+1, 2) - tir.floordiv(i, 2), 0),
             tir.LE(tir.floordiv(i+1, 2) - tir.floordiv(i, 2), 1)
         )),
         expected=True)

# ============================================================================
section("类别 8: 挑战 CanProve 极限的场景")
# 构造理论上标准 CanProve 难以处理的场景
# ============================================================================

i = tir.Var('i', 'int32')
j = tir.Var('j', 'int32')
a = tir.Var('a', 'int32')
b = tir.Var('b', 'int32')
c = tir.Var('c', 'int32')

# 非线性 + 多变量
with analyzer.constraint_scope(tir.And(a >= 2, a <= 10)):
    with analyzer.constraint_scope(tir.And(b >= 2, b <= 10)):
        with analyzer.constraint_scope(tir.And(c >= 2, c <= 10)):
            # 三变量乘积边界
            test("a*b*c <= 1000 (=10*10*10)",
                 analyzer.can_prove(tir.LE(a*b*c, 1000)),
                 expected=True)
            
            test("a*b*c < 1001",
                 analyzer.can_prove(tir.LT(a*b*c, 1001)),
                 expected=True)

# 多项式约束
with analyzer.constraint_scope(tir.And(i >= 0, i < 1024)):
    # i*i 的范围 [0, 1023*1023] = [0, 1046529]
    test("i*i < 1047000",
         analyzer.can_prove(tir.LT(i*i, 1047000)),
         expected=True)
    
    test("i*i + 2*i + 1 == (i+1)*(i+1)（平方公式）",
         analyzer.can_prove(tir.EQ(i*i + 2*i + 1, (i+1)*(i+1))),
         expected=True)

# ============================================================================
section("类别 9: Ascend 同步插入的真实痛点 - 切片访问")
# tvm_access_ptr 的 offset/extent 是符号表达式时
# ============================================================================

offset1 = tir.Var('offset1', 'int32')
offset2 = tir.Var('offset2', 'int32')
extent = tir.Var('extent', 'int32')
buf_size = tir.Var('buf_size', 'int32')

# 已知：offset1 + extent == offset2（两个切片紧邻）
# 问：两个切片是否不重叠？
with analyzer.constraint_scope(tir.And(offset1 >= 0, buf_size >= 0)):
    with analyzer.constraint_scope(tir.EQ(offset2, offset1 + extent)):
        # 切片 1: [offset1, offset1+extent)
        # 切片 2: [offset2, offset2+extent) = [offset1+extent, offset1+2*extent)
        # 不重叠条件：offset1 + extent <= offset1 + extent（边界相等，不重叠）
        end1 = offset1 + extent
        start2 = offset2  # = offset1 + extent
        test("切片紧邻: end1 <= start2",
             analyzer.can_prove(tir.LE(end1, start2)),
             expected=True)
        
        # 重叠情况：offset2 = offset1 + extent/2（一半重叠）
        pass

with analyzer.constraint_scope(tir.And(offset1 >= 0, offset1 < 1024)):
    with analyzer.constraint_scope(tir.And(extent >= 32, extent <= 512)):
        with analyzer.constraint_scope(tir.EQ(offset2, offset1 + extent)):
            end1 = offset1 + extent
            start2 = offset2
            test("切片紧邻 + range: end1 <= start2",
                 analyzer.can_prove(tir.LE(end1, start2)),
                 expected=True)

# ============================================================================
print(f"\n{'='*70}")
print("  总结")
print(f"{'='*70}")
print("""
观察：
- 标准 CanProve 已覆盖几乎所有 Ascend 业务场景
- 即使复杂模算术、深层嵌套、非线性约束也能证明
- Z3 在 Ascend 场景的真正价值点需要进一步挖掘

建议：
1. 优先使用标准 CanProve（轻量、快速）
2. 在标准 CanProve 无法求解时，再考虑引入 Z3 作为兜底
3. 关注\"极端边界情况\"：多层 if + 非线性 + 模嵌套的真实案例
""")
