#!/usr/bin/env python3
"""
Z3 移植验证脚本：测试 TileLang-Ascend 标准 CanProve API 的证明能力

测试四个核心场景：
1. For 循环：相邻迭代访问不重叠
2. If-Else：分支内访问与外部无冲突
3. 切片访问：字节范围不重叠
4. 跨线程建模：WAW / RAW / WAR / RAR 依赖分析
"""

import sys
sys.path.insert(0, '/mnt/workspace/developer/workspace/git/github/erhsh/tilelang-ascend')

import tilelang
import tvm
import tvm.arith
import tvm.tirx as tir
from tvm.arith import Analyzer, ProofStrength


def test_section(title):
    """打印段落标题"""
    print(f"\n{'='*70}")
    print(f"  {title}")
    print(f"{'='*70}\n")


def test_result(description, can_prove, strength_name):
    """打印测试结果"""
    status = "✓ PROVED" if can_prove else "✗ CANNOT PROVE"
    print(f"[{status}] {description} ({strength_name})")
    return can_prove


# ============================================================================
# 场景 1: For 循环 - 相邻迭代访问不重叠
# ============================================================================
test_section("场景 1: For 循环 - 相邻迭代访问不重叠")

analyzer = Analyzer()

# 1.1 基础数学等式
i = tir.Var('i', 'int32')

test_result(
    "i == i (自反性)",
    analyzer.can_prove(tir.EQ(i, i)),
    "DEFAULT"
)

test_result(
    "i != i + 1 (相邻迭代不重叠)",
    analyzer.can_prove(tir.NE(i, i + 1)),
    "DEFAULT"
)

# 1.2 带循环变量范围约束
N = 16
i_bounded = tir.Var('i', 'int32')

# 模拟 for (i=0; i<16; i++)
with analyzer.constraint_scope(tir.And(i_bounded >= 0, i_bounded < N)):
    # 访问模式：A[i] vs A[i+1]
    test_result(
        f"i in [0, {N-1}]: i != i + 1",
        analyzer.can_prove(tir.NE(i_bounded, i_bounded + 1)),
        "DEFAULT"
    )
    
    # 字节偏移：假设 4 字节元素
    test_result(
        f"i in [0, {N-1}]: 4*i != 4*(i+1)",
        analyzer.can_prove(tir.NE(4 * i_bounded, 4 * (i_bounded + 1))),
        "DEFAULT"
    )
    
    # 访问模式：A[i] vs A[i-1] (相邻迭代反向)
    test_result(
        f"i in [0, {N-1}]: i != i - 1",
        analyzer.can_prove(tir.NE(i_bounded, i_bounded - 1)),
        "DEFAULT"
    )

# 1.3 带符号边界强度
test_result(
    "i != i + 1 (kSymbolicBound)",
    analyzer.can_prove(tir.NE(i, i + 1), ProofStrength.SYMBOLIC_BOUND),
    "SYMBOLIC_BOUND"
)

# 1.4 非相邻迭代（距离 k）
for k in [2, 4, 8]:
    test_result(
        f"i != i + {k} (距离 {k})",
        analyzer.can_prove(tir.NE(i, i + k)),
        "DEFAULT"
    )


# ============================================================================
# 场景 2: If-Else - 分支访问无冲突
# ============================================================================
test_section("场景 2: If-Else - 分支访问无冲突")

analyzer = Analyzer()

# 2.1 简单范围不重叠
# 模拟：访问范围 [0, 128) 和 [128, 256) 不重叠
addr = tir.Var('addr', 'int32')
extent1, extent2 = 128, 128

test_result(
    f"addr + {extent1} <= addr + {extent1 + extent2}",
    analyzer.can_prove(tir.LE(addr + extent1, addr + extent1 + extent2)),
    "DEFAULT"
)

# 2.2 两个访问范围不重叠判断
# access1: [base1, base1+size1)
# access2: [base2, base2+size2)
# 不重叠条件：base1+size1 <= base2 || base2+size2 <= base1
base1 = tir.Var('base1', 'int32')
base2 = tir.Var('base2', 'int32')
size1 = 128
size2 = 128

# 已知 base2 == base1 + size1 (连续分布)
with analyzer.constraint_scope(tir.EQ(base2, base1 + size1)):
    test_result(
        f"连续分布: base1+{size1} <= base2 (不重叠)",
        analyzer.can_prove(tir.LE(base1 + size1, base2)),
        "DEFAULT"
    )

# 2.3 条件蕴含关系测试
# 模拟：条件 A ⟹ 条件 B
x = tir.Var('x', 'int32')

# A: x > 10, B: x >= 5
# A ⟹ B 等价于 Not(A) Or B == x <= 10 Or x >= 5
with analyzer.constraint_scope(x > 10):
    test_result(
        "x > 10 ⟹ x >= 5",
        analyzer.can_prove(x >= 5),
        "DEFAULT"
    )

# 2.4 反向不成立
with analyzer.constraint_scope(x >= 5):
    result = test_result(
        "x >= 5 ⟹ x > 10 (预期失败)",
        analyzer.can_prove(x > 10),
        "DEFAULT"
    )
    if result:
        print("  ⚠️ 意外通过，可能存在问题")


# ============================================================================
# 场景 3: 切片访问 - 字节范围不重叠
# ============================================================================
test_section("场景 3: 切片访问 - 字节范围不重叠")

analyzer = Analyzer()

# 3.1 模拟 tvm_access_ptr 的 offset/extent 分析
# buf1: offset=0, extent=128, dtype=int32 (4字节)
# buf2: offset=128, extent=128, dtype=int32
# 字节范围：buf1[0, 512), buf2[512, 1024) → 不重叠

offset1 = tir.Var('offset1', 'int32')
offset2 = tir.Var('offset2', 'int32')
extent = 128
dtype_bytes = 4

# 已知 offset2 == offset1 + extent (连续切片)
with analyzer.constraint_scope(tir.EQ(offset2, offset1 + extent)):
    # 字节范围：[offset1*4, offset1*4 + 512) 和 [offset2*4, offset2*4 + 512)
    # 不重叠条件：offset1*4 + 512 <= offset2*4
    byte_end1 = offset1 * dtype_bytes + extent * dtype_bytes
    byte_start2 = offset2 * dtype_bytes
    
    test_result(
        f"切片 offset2=offset1+{extent}: end1 <= start2 (不重叠)",
        analyzer.can_prove(tir.LE(byte_end1, byte_start2)),
        "DEFAULT"
    )

# 3.2 部分重叠场景（预期无法证明不重叠）
with analyzer.constraint_scope(tir.EQ(offset2, offset1 + extent // 2)):
    byte_end1 = offset1 * dtype_bytes + extent * dtype_bytes
    byte_start2 = offset2 * dtype_bytes
    
    result = test_result(
        f"切片 offset2=offset1+{extent//2}: end1 <= start2 (预期失败，实际重叠)",
        analyzer.can_prove(tir.LE(byte_end1, byte_start2)),
        "DEFAULT"
    )
    if result:
        print("  ⚠️ 意外通过，可能存在 bug")


# ============================================================================
# 场景 4: 跨线程建模 - WAW / RAW / WAR / RAR
# ============================================================================
test_section("场景 4: 跨线程建模 - WAW / RAW / WAR / RAR")

analyzer = Analyzer()

# 4.1 WAW (同线程) - 使用相同变量
tx = tir.Var('tx', 'int32')

with analyzer.constraint_scope(tir.And(tx >= 0, tx < 32)):
    test_result(
        "WAW: 同线程 tx 访问 A[tx] == A[tx] (冲突)",
        analyzer.can_prove(tir.EQ(tx, tx)),
        "DEFAULT"
    )

# 4.2 RAR (不同线程) - 使用不同变量
tx1 = tir.Var('tx1', 'int32')
tx2 = tir.Var('tx2', 'int32')

with analyzer.constraint_scope(tir.And(
    tir.And(tx1 >= 0, tx1 < 32),
    tir.And(tx2 >= 0, tx2 < 32)
)):
    # 如果 tx1 != tx2，则访问不重叠
    with analyzer.constraint_scope(tir.NE(tx1, tx2)):
        test_result(
            "RAR: 不同线程 tx1 != tx2 ⟹ A[tx1] != A[tx2]",
            analyzer.can_prove(tir.NE(tx1, tx2)),
            "DEFAULT"
        )

# 4.3 RAW (写后读) - 同线程
with analyzer.constraint_scope(tir.And(tx >= 0, tx < 32)):
    test_result(
        "RAW: 同线程写 A[tx] 后读 A[tx] (相同)",
        analyzer.can_prove(tir.EQ(tx, tx)),
        "DEFAULT"
    )

# 4.4 WAR (读后写) - 不同线程
with analyzer.constraint_scope(tir.And(
    tir.And(tx1 >= 0, tx1 < 32),
    tir.And(tx2 >= 0, tx2 < 32)
)):
    with analyzer.constraint_scope(tir.NE(tx1, tx2)):
        test_result(
            "WAR: 线程 tx1 读 A[tx1]，线程 tx2 写 A[tx2]，tx1!=tx2 (不重叠)",
            analyzer.can_prove(tir.NE(tx1, tx2)),
            "DEFAULT"
        )


# ============================================================================
# 场景 5: 复杂表达式 - 模运算、FloorDiv
# ============================================================================
test_section("场景 5: 复杂表达式 - 模运算、FloorDiv")

analyzer = Analyzer()

# 5.1 模运算
i = tir.Var('i', 'int32')

with analyzer.constraint_scope(tir.And(i >= 0, i < 16)):
    test_result(
        "i % 2 in [0, 1]",
        analyzer.can_prove(tir.And(tir.GE(tir.floormod(i, 2), 0), 
                                    tir.LE(tir.floormod(i, 2), 1))),
        "DEFAULT"
    )
    
    # i % 2 != (i+1) % 2 (对于连续 i)
    test_result(
        "i % 2 != (i+1) % 2",
        analyzer.can_prove(tir.NE(tir.floormod(i, 2), 
                                   tir.floormod(i + 1, 2))),
        "DEFAULT"
    )

# 5.2 FloorDiv
with analyzer.constraint_scope(tir.And(i >= 0, i < 128)):
    test_result(
        "floor_div(i, 4) in [0, 31]",
        analyzer.can_prove(tir.And(tir.GE(tir.floordiv(i, 4), 0),
                                    tir.LE(tir.floordiv(i, 4), 31))),
        "DEFAULT"
    )


# ============================================================================
# 总结报告
# ============================================================================
test_section("验证总结")

print("""
标准 CanProve API 能力评估：

✓ 可以证明的场景：
  1. 简单等式/不等式：i == i, i != i+1
  2. 带范围约束的线性表达式
  3. 字节偏移不重叠（已知 offset 关系时）
  4. 条件蕴含（简单情形）
  5. 跨线程依赖（已知 tx1 != tx2）

✗ 可能无法证明的场景（需要 Z3）：
  1. 复杂约束等价性（双向蕴含）
  2. 非线性表达式边界
  3. 复杂模运算性质
  4. 多变量耦合约束

建议：
  - 对简单场景可直接使用标准 CanProve
  - 对复杂场景需引入 Z3Prover 或使用 kSymbolicBound 强度
  - 可先实现保守策略，逐步放宽精度
""")

print("\n验证完成！")
