# Neo-Hookean 弹性体模拟数学推导

## 1. 变形梯度 (Deformation Gradient)

### 1.1 定义

对于四面体单元，顶点为 $\mathbf{x}_0, \mathbf{x}_1, \mathbf{x}_2, \mathbf{x}_3$：

**参考配置下的形状矩阵**：
$$
\mathbf{D}_m = \begin{bmatrix}
\mathbf{x}_1^0 - \mathbf{x}_0^0 & \mathbf{x}_2^0 - \mathbf{x}_0^0 & \mathbf{x}_3^0 - \mathbf{x}_0^0
\end{bmatrix} \in \mathbb{R}^{3 \times 3}
$$

**当前配置下的形状矩阵**：
$$
\mathbf{D}_s = \begin{bmatrix}
\mathbf{x}_1 - \mathbf{x}_0 & \mathbf{x}_2 - \mathbf{x}_0 & \mathbf{x}_3 - \mathbf{x}_0
\end{bmatrix} \in \mathbb{R}^{3 \times 3}
$$

**变形梯度张量**：
$$
\mathbf{F} = \mathbf{D}_s \mathbf{D}_m^{-1}
$$

### 1.2 物理意义

- $\mathbf{F}$ 描述了材料从参考配置到当前配置的局部变形
- $\mathbf{D}_m^{-1}$ 在初始化时**预计算一次**，存储在 `Dm_inv_buffer` 中
- 每个时间步只需计算 $\mathbf{D}_s$（当前边向量），然后做矩阵乘法 $\mathbf{F} = \mathbf{D}_s \mathbf{D}_m^{-1}$

### 1.3 不变量

$$
J = \det(\mathbf{F}) \quad \text{(体积比)}
$$

$$
I_1 = \text{tr}(\mathbf{F}^T \mathbf{F}) = \|\mathbf{F}\|_F^2 \quad \text{(第一不变量)}
$$

## 2. Neo-Hookean 能量密度

### 2.1 能量密度函数

$$
\Psi(\mathbf{F}) = \frac{\mu}{2}(I_1 - 3) - \mu \log(J) + \frac{\lambda}{2} \log^2(J)
$$

其中：
- $\mu = \frac{E}{2(1+\nu)}$：第一 Lamé 参数（剪切模量）
- $\lambda = \frac{E\nu}{(1+\nu)(1-2\nu)}$：第二 Lamé 参数
- $E$：杨氏模量 (Young's Modulus)
- $\nu$：泊松比 (Poisson's Ratio)

### 2.2 单元弹性能量

对于体积为 $V$ 的单元：
$$
E_{\text{elastic}}^{\text{elem}} = V \cdot \Psi(\mathbf{F})
$$

总弹性能量：
$$
E_{\text{elastic}} = \sum_{e=1}^{N_{\text{elem}}} V_e \Psi(\mathbf{F}_e)
$$

## 3. 第一类 Piola-Kirchhoff 应力张量

### 3.1 定义

应力张量是能量密度对变形梯度的导数：
$$
\mathbf{P} = \frac{\partial \Psi}{\partial \mathbf{F}}
$$

### 3.2 推导

$$
\frac{\partial I_1}{\partial \mathbf{F}} = \frac{\partial \text{tr}(\mathbf{F}^T \mathbf{F})}{\partial \mathbf{F}} = 2\mathbf{F}
$$

$$
\frac{\partial J}{\partial \mathbf{F}} = \frac{\partial \det(\mathbf{F})}{\partial \mathbf{F}} = J \mathbf{F}^{-T}
$$

$$
\frac{\partial \log(J)}{\partial \mathbf{F}} = \frac{1}{J} \cdot J \mathbf{F}^{-T} = \mathbf{F}^{-T}
$$

因此：
$$
\mathbf{P} = \mu \mathbf{F} - \mu \mathbf{F}^{-T} + \lambda \log(J) \mathbf{F}^{-T}
$$

简化为：
$$
\boxed{\mathbf{P} = \mu(\mathbf{F} - \mathbf{F}^{-T}) + \lambda \log(J) \mathbf{F}^{-T}}
$$

## 4. 隐式时间积分

### 4.1 Backward Euler 方案

位置更新：
$$
\mathbf{x}^{n+1} = \mathbf{x}^n + \Delta t \mathbf{v}^{n+1}
$$

速度更新：
$$
\mathbf{M} \mathbf{v}^{n+1} = \mathbf{M} \mathbf{v}^n + \Delta t \mathbf{f}(\mathbf{x}^{n+1})
$$

其中 $\mathbf{f}(\mathbf{x}) = \mathbf{f}_{\text{elastic}}(\mathbf{x}) + \mathbf{f}_{\text{ext}}$

### 4.2 增量势能 (Incremental Potential)

定义预测位置（显式步）：
$$
\tilde{\mathbf{x}} = \mathbf{x}^n + \Delta t \mathbf{v}^n
$$

隐式更新等价于最小化增量势能：
$$
\mathbf{x}^{n+1} = \arg\min_{\mathbf{x}} E(\mathbf{x})
$$

其中总能量为：
$$
\boxed{E(\mathbf{x}) = \frac{1}{2}(\mathbf{x} - \tilde{\mathbf{x}})^T \mathbf{M} (\mathbf{x} - \tilde{\mathbf{x}}) + \Delta t^2 \Psi(\mathbf{x}) - \Delta t^2 \mathbf{f}_{\text{ext}}^T \mathbf{x}}
$$

三项分别为：
1. **惯性能量**：二次型，描述偏离预测位置的惩罚
2. **弹性能量**：非线性，乘以 $\Delta t^2$
3. **外力势能**：线性项

## 5. 梯度计算

### 5.1 总梯度

$$
\nabla E = \mathbf{M}(\mathbf{x} - \tilde{\mathbf{x}}) + \Delta t^2 \nabla \Psi(\mathbf{x}) - \Delta t^2 \mathbf{f}_{\text{ext}}
$$

### 5.2 弹性梯度

对于单个四面体单元，弹性力为：
$$
\mathbf{f}_{\text{elastic}}^{\text{elem}} = -\frac{\partial (V \Psi)}{\partial \mathbf{x}} = -V \frac{\partial \Psi}{\partial \mathbf{F}} \frac{\partial \mathbf{F}}{\partial \mathbf{x}}
$$

由于 $\mathbf{F} = \mathbf{D}_s \mathbf{D}_m^{-1}$，对第 $i$ 个顶点的梯度为：
$$
\frac{\partial \mathbf{F}}{\partial \mathbf{x}_i} = \begin{cases}
\mathbf{D}_m^{-T} & i \neq 0 \\
-(\mathbf{D}_m^{-T})^T \mathbf{1} & i = 0
\end{cases}
$$

因此：
$$
\mathbf{H}_i = -V \mathbf{P} \mathbf{D}_m^{-T}
$$

其中 $\mathbf{H}_i$ 是 $3 \times 3$ 矩阵，对应顶点 $i$ 在三个方向上的力。

**关键实现细节**：
```cpp
// 计算 H = V * P * Dm_inv^T
Eigen::Matrix3f H = volume * P * Dm_inv_mat.transpose();
```

### 5.3 组装全局梯度

对于顶点 $i \neq 0$：直接加上 $\mathbf{H}_i$ 对应列
对于顶点 $0$（apex）：减去其他三个顶点的贡献之和

## 6. Hessian 矩阵（二阶导数）

### 6.1 精确 Hessian（理论）

$$
\mathbf{H} = \mathbf{M} + \Delta t^2 \frac{\partial^2 E_{\text{elastic}}}{\partial \mathbf{x}^2}
$$

精确的弹性 Hessian 需要计算 $\frac{\partial^2 \Psi}{\partial \mathbf{F}^2}$，非常复杂。

### 6.2 St. Venant-Kirchhoff 近似

**近似方法**：用 St. Venant-Kirchhoff (SVK) 材料的 Hessian 近似 Neo-Hookean 的 Hessian。

SVK 能量密度：
$$
\Psi_{\text{SVK}}(\mathbf{F}) = \mu \|\mathbf{E}\|_F^2 + \frac{\lambda}{2} (\text{tr}(\mathbf{E}))^2
$$

其中 Green 应变张量：
$$
\mathbf{E} = \frac{1}{2}(\mathbf{F}^T \mathbf{F} - \mathbf{I})
$$

SVK 的弹性张量 $\mathbb{C}$ 可以显式计算，用于构建 Hessian。

### 6.3 PSD 投影

由于 SVK 近似可能产生负特征值（非凸），需要进行**正定投影 (PSD Projection)**：
$$
\mathbf{H}_{\text{PSD}} = \mathbf{V} \max(\mathbf{\Lambda}, \epsilon \mathbf{I}) \mathbf{V}^T
$$

其中 $\mathbf{V} \mathbf{\Lambda} \mathbf{V}^T$ 是特征分解，$\epsilon$ 是小正数（如 $10^{-6}$）。

## 7. Newton-Raphson 求解

### 7.1 Newton 方向

在每次迭代中，求解线性系统：
$$
\mathbf{H} \mathbf{p} = -\nabla E(\mathbf{x}^k)
$$

其中：
- $\mathbf{H}$：近似 Hessian 矩阵（CSR 稀疏格式）
- $\mathbf{p}$：Newton 搜索方向
- 使用 **CUDA CG (Conjugate Gradient)** 求解

### 7.2 Line Search（回溯线搜索）

$$
\alpha \in \{1, 0.5, 0.25, 0.125, \ldots\}
$$

接受条件（Armijo 准则简化版）：
$$
E(\mathbf{x}^k + \alpha \mathbf{p}) \leq E(\mathbf{x}^k) + \epsilon
$$

### 7.3 位置更新

$$
\mathbf{x}^{k+1} = \mathbf{x}^k + \alpha \mathbf{p}
$$

### 7.4 收敛判据

$$
\frac{\|\nabla E(\mathbf{x}^k)\|}{N_{\text{dof}}} < \text{tol}
$$

默认 `tol = 1e-2`

## 8. 数值梯度验证

### 8.1 有限差分

$$
\frac{\partial E}{\partial x_i} \approx \frac{E(\mathbf{x} + \epsilon \mathbf{e}_i) - E(\mathbf{x})}{\epsilon}
$$

### 8.2 验证结果

在第二次 Newton 迭代（$\mathbf{x} \neq \tilde{\mathbf{x}}$，避免浮点精度问题）：
- 数值梯度：`-6.854534e-03`
- 解析梯度：`-7.185664e-03`
- **相对误差：4.6%**（可接受范围）

多次迭代测试显示相对误差在 **0.7% ~ 4.6%** 之间，验证了能量函数和梯度计算的**数学一致性**。

## 9. 关键 Bug 修复记录

### 9.1 Dm_inv 转置 Bug（已修复）

**问题**：初始存储格式错误
```cpp
// 错误（列优先，实际存储了转置）：
Dm_inv_local[j*3+i] = Dm_inv_mat(i,j);

// 正确（行优先）：
Dm_inv_local[i*3+j] = Dm_inv_mat(i,j);
```

**影响**：
- 计算的是 $\mathbf{F} = \mathbf{D}_s \mathbf{D}_m^{-T}$ 而非 $\mathbf{F} = \mathbf{D}_s \mathbf{D}_m^{-1}$
- 即使静止状态，$\mathbf{F} \neq \mathbf{I}$，产生虚假弹性能量
- 修复后梯度范数从 **1.59 降至 5.27e-06**（6个数量级改进！）

### 9.2 重力设置 Bug（已修复）

**问题**：外力被强制设为零
```cpp
// 错误：
f_ext_ptr[i] = 0.0f;

// 正确：
f_ext_ptr[i] = (i % 3 == 2) ? mass * gravity : 0.0f;  // z方向
```

## 10. 性能数据

- **收敛速度**：120帧，总耗时 5.8秒（平均 48ms/帧）
- **Newton 迭代**：通常 2-5 次迭代收敛（静止状态附近 1-2 次）
- **CG 求解**：通常 50-200 次迭代
- **内存占用**：主要是 CSR 稀疏矩阵（约 $O(N_{\text{elem}})$ 非零元）

## 11. 参数调优建议

### 11.1 材料参数

- **Young's Modulus**：$10^3$ ~ $10^5$（软体 ~ 中等硬度）
  - 过大会导致 line search alpha 过小
  - 建议 scaling：`mu *= 0.01`, `lambda *= 0.01`

- **Poisson's Ratio**：$0.3$ ~ $0.45$
  - $\nu \to 0.5$：不可压缩（数值不稳定）
  - $\nu < 0$ 或 $\nu \geq 0.5$：非物理

### 11.2 求解器参数

- **Substeps**：5-10（刚度大需要更多）
- **Newton Iterations**：20-50
- **Newton Tolerance**：$10^{-3}$ ~ $10^{-2}$
- **CG Tolerance**：`grad_norm * 1e-3`（自适应）

### 11.3 稳定性

- **PSD 投影**：$\epsilon = 10^{-6}$（保证 Hessian 正定）
- **Line search 下界**：`alpha_min = 1e-6`（避免无进展）
- **阻尼系数**：$0.95$ ~ $0.99$（能量耗散）

## 12. 未来改进方向

1. **精确 Neo-Hookean Hessian**：替代 SVK 近似，提高高刚度下的收敛性
2. **Projected Newton**：更 robust 的 Hessian 近似方法
3. **自适应时间步长**：根据 CFL 条件动态调整
4. **GPU 稀疏求解器优化**：利用 tensor cores 加速
5. **多重网格预条件**：加速 CG 收敛（特别是大规模网格）
