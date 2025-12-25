这是一份基于我们从“完成 HZB 功能”这一里程碑之后，针对后续 SSGI 与 SSR 开发内容的**系统性梳理与总结**。

这份整理旨在帮助你理清思路，明确技术原理，并锁定接下来的开发任务。

------

### 📅 **当前状态：里程碑确认**

- **已完成**：Hi-Z (Hierarchical Z-Buffer) 纹理生成。
- **已验证**：通过 RenderDoc 确认了 Mip 链的正确性（Max Depth, Reverse-Z）。
- **当前阶段**：准备利用 HZB 进行核心的光线追踪计算。

------

### 1. **核心架构设计：一鱼两吃**

我们确定了**SSGI (漫反射)** 和 **SSR (镜面反射)** 是“孪生兄弟”，它们共享 90% 的底层逻辑。

- **设计原则**：不要写两套独立代码，而是构建一个**“公共底座，双轨并行”**的架构。
- **公共底座**：
  - **HZB Generation**：每一帧只算一次，结果共享。
  - **Ray Tracing Kernel**：`HiZTrace` 函数（负责在屏幕空间光线求交）是通用的。
- **差异点**：
  - **SSGI**：光线沿**半球随机方向**发射，计算 Indirect Diffuse。
  - **SSR**：光线沿**反射方向**发射，计算 Indirect Specular。

------

### 2. **关键理论辨析 (结合 Games202)**

针对你关于“Ray Tracing 定义”和“Lumen 原理”的困惑，我们进行了深度的理论对齐：

#### A. SSGI 本质就是 Ray Tracing

- **结论**：你的 SSGI 项目在算法本质上就是 **Real-Time Ray Tracing**（尽管是在 Screen Space 进行的）。
- **推论**：既然是 Ray Tracing 且只有 1 SPP（每像素 1 根光线），那么 Games202 Lecture 13 中提到的**降噪（Denoising）是必须的**。
  - 如果不降噪，结果就是满屏的噪点（Variance）。
  - **空间滤波 (Spatial)**：必须做（如双边滤波）。
  - **时域累积 (Temporal)**：推荐借用 UE 的 TAA，而不是手写复杂的 History Buffer。

#### B. Lumen 的“分频”策略

我们分析了为什么 UE 将 GI 和 Reflection 分开设置：

- **漫反射 (GI)** = **低频信号**。Lumen 使用 **Surface Cache + Radiance Cache**（低分率探针）来“偷懒”和复用。
- **镜面反射 (Reflection)** = **高频信号**。Lumen 使用 **高精度追踪 (SSR -> SDF -> Hardware)** 混合策略。
- **你的定位**：你的项目是在手动实现 **Lumen 反射管线的 Level 1 (Screen Traces)** 和 **无缓存版的 GI**。

------

### 3. **实战开发路线图 (Roadmap)**

这是接下来的具体执行清单，按优先级排序：

#### 🔧 **阶段一：核心引擎 (Ray Tracing Core)**

- **任务**：实现通用的光线步进器。
- **文件**：`RayTracingCommon.ush`
- **核心函数**：`HiZTrace`
  - **输入**：起点、终点、HZB 纹理。
  - **逻辑**：在 HZB 的 Mip 间跳跃（Cell Jumping），实现 $O(\log N)$ 加速。
  - **注意**：严格遵守 UE 的 Reverse-Z（近 1.0，远 0.0）判定逻辑。

#### 🎨 **阶段二：功能实现 (Shading)**

- **任务**：让画面出现正确的间接光（红墙映红地）。
- **文件**：`SSGI.usf`
- **逻辑**：
  1. 重建世界坐标 (World Pos)。
  2. 读取 GBuffer 法线。
  3. **半球随机采样**：生成随机光线方向。
  4. 调用 `HiZTrace`。
  5. 输出 **Raw Noisy Color**（此时全是噪点，这是正常的）。

#### 🌫️ **阶段三：降噪与合成 (Denoise & Composite)**

- **任务**：把噪点变成柔和的光照。
- **文件**：`SSGI_Blur.usf`
- **逻辑**：
  - 实现 **联合双边滤波 (Joint Bilateral Filter)**。
  - 利用 **Depth** 和 **Normal** 权重防止模糊越过物体边缘。
- **最终合成**：将降噪后的结果混合回 `SceneColor`，并让 UE 的 **TAA** 完成最后的平滑。

------

### 4. **项目文件结构总览**

Plaintext

```
Plugins/HZB_SSGI/
├── Source/HZB_SSGI/Private/
│   └── HZBSSGISceneViewExtension.cpp  <-- [C++ 总控] 串联 Pass
│
└── Shaders/Private/
    ├── HZB.usf                  <-- [已完成] 生成 Hi-Z
    ├── RayTracingCommon.ush     <-- [核心库] HiZTrace 函数
    ├── SSGI.usf                 <-- [功能] RayMarching + Shading
    ├── SSGI_Blur.usf            <-- [优化] Bilateral Denoise
    └── SSR.usf                  <-- [进阶可选] 镜面反射逻辑
```

### 5. **下一步立即行动**

不要试图一口气写完所有代码。请按照我们最后确定的微观计划执行：

1. **创建 `RayTracingCommon.ush`**：把 `HiZTrace` 函数填进去。
2. **创建 `SSGI.usf`**：写一个最简单的 MainCS，先尝试调用 `HiZTrace` 并输出简单的 Debug 颜色（例如：命中输出绿色，未命中输出红色），验证光线是否真的在场景里“飞”起来了。

整理完毕，随时可以开始写代码！



基于我们之前的对话以及你已经完成的 HZB（Hierarchical Z-Buffer）实现，以下是你接下来的 **HZB_SSGI** 项目开发路线图。

既然你已经验证了 Hi-Z 纹理生成的正确性，现在的核心任务是将这个“加速结构”用于真正的光线追踪，并处理随之而来的噪点问题。

我们将后续工作分为三个阶段：**光线追踪核心 (Ray Tracing)**、**降噪处理 (Denoising)** 和 **最终合成 (Compositing)**。

------

### 第一阶段：SSGI 核心实现 (Ray Tracing)

这是实现“红墙映红地板”效果的关键步骤。你需要编写光线步进逻辑，并在 Compute Shader 中发射光线。

**1. 创建公共函数库 (`RayTracingCommon.ush`)**

- **目的**：将核心的 Hi-Z 追踪逻辑独立出来，方便后续复用于 SSR。
- **核心函数 `HiZTrace`**：
  - 输入：光线起点 (View Space/Screen Space)、光线方向、HZB 纹理、最大迭代次数。
  - 逻辑：在 Hi-Z 的不同 Mip 层级间跳跃（Cell Jumping）。如果当前层级没撞到物体，就去更高层级（跳过大片区域）；如果撞到了，就去更低层级（精细检查）。
  - 输出：命中点的 UV 坐标和深度。

**2. 编写 SSGI Shader (`SSGI.usf`)**

- **目的**：计算间接漫反射 (Indirect Diffuse)。
- **主要逻辑 (`MainCS`)**：
  1. **世界坐标重建**：根据当前像素的 UV 和 DeviceDepth，还原出 **World Position**。
  2. **获取几何信息**：从 GBuffer 读取 **World Normal**。
  3. **生成随机光线**：实现 **Cosine-Weighted Hemisphere Sampling**（余弦加权半球采样）。你需要一个随机数生成器（可以使用简单的 Hash 函数或 Blue Noise 纹理），基于法线生成一个随机方向。
  4. **执行追踪**：调用 `HiZTrace`。
  5. **采样颜色**：如果追踪命中（Hit），采样上一帧的 `SceneColor`（或者当前帧的，如果无法获取上一帧）。
  6. **输出结果**：将采样到的颜色写入 `SSGI_Raw_Output` (UAV)。此时的结果会充满噪点。

**3. 更新 C++ 逻辑 (`HZBSSGISceneViewExtension.cpp`)**

- **任务**：在 HZB 循环结束后，插入 SSGI Pass。
- **操作**：
  - 创建 `SSGI_Raw` 纹理。
  - 绑定 HZB 纹理、GBuffer、SceneColor 等参数。
  - 调用 `AddComputePass` 执行 `SSGI.usf`。

------

### 第二阶段：空间降噪 (Spatial Denoising)

正如 Games202 课程所述，1 SPP（每像素 1 根光线）的 Ray Tracing 结果是不可用的，必须进行降噪。

**1. 编写降噪 Shader (`SSGI_Blur.usf`)**

- **目的**：实现 **联合双边滤波 (Joint Bilateral Filter)**，抹平噪点但保留边缘。
- **主要逻辑 (`MainCS`)**：
  - 输入：`SSGI_Raw` 纹理、`SceneDepth`、`GBuffer` (Normal)。
  - 算法：对当前像素周围（如 9x9 或 5x5）区域进行加权平均。
  - **权重计算**：
    - **空间权重**：距离越远，权重越小（高斯）。
    - **深度权重**：深度差异过大（边缘），权重归零。
    - **法线权重**：法线角度差异过大，权重归零。
  - 输出：写入 `SSGI_Denoised` 纹理。

**2. 更新 C++ 逻辑**

- **任务**：在 SSGI Pass 之后，插入 Blur Pass。
- **操作**：
  - 创建 `SSGI_Denoised` 纹理。
  - 将 `SSGI_Raw` 作为 SRV 输入，`SSGI_Denoised` 作为 UAV 输出。
  - 调用 `AddComputePass` 执行 `SSGI_Blur.usf`。

------

### 第三阶段：混合与时域稳定 (Compositing & TAA)

将计算好的间接光叠加到画面上，并利用 UE 的 TAA 进行最终修饰。

**1. 编写混合 Shader (或是简单的 Copy Pass)**

- **目的**：将 SSGI 结果叠加到 SceneColor。
- **逻辑**：
  - 简单叠加：`FinalColor = SceneColor + SSGI_Denoised * DiffuseColor`。
  - PBR 叠加（进阶）：根据 Roughness 和 Metallic 调整 SSGI 的强度（金属通常没有漫反射 GI）。

**2. 利用 UE 原生 TAA**

- **策略**：你不需要手写 Temporal Accumulation。
- **原理**：因为你的 Pass 插入在 `SSRInput` 阶段，后续 UE 会自动执行 TAA Pass。只要你把降噪后的 SSGI 混合进了 SceneColor，TAA 就会自动帮你处理剩余的闪烁和抖动。

------

### 总结：你的文件与任务清单

| **阶段**    | **文件名 (建议)**               | **核心任务**                              | **验证标准**                             |
| ----------- | ------------------------------- | ----------------------------------------- | ---------------------------------------- |
| **0. 基础** | `HZB.usf` / `.cpp`              | **已完成** (生成 Hi-Z Mip 链)             | RenderDoc 查看 Mip 图                    |
| **1. 核心** | `RayTracingCommon.ush`          | 实现 `HiZTrace` 函数                      | 无单独验证                               |
|             | `SSGI.usf`                      | 实现半球采样 + 调用 Trace + 输出 Raw 颜色 | **屏幕出现正确的颜色溢出（但全是噪点）** |
|             | `HZBSSGISceneViewExtension.cpp` | 添加 SSGI Dispatch Pass                   | 代码跑通不崩溃                           |
| **2. 降噪** | `SSGI_Blur.usf`                 | 实现双边滤波 (Bilateral Filter)           | **噪点变模糊，边缘依然清晰**             |
|             | `HZBSSGISceneViewExtension.cpp` | 添加 Blur Dispatch Pass                   |                                          |
| **3. 合成** | `Composite.usf` (可选)          | 将 SSGI 混合回 SceneColor                 | **最终画面变亮，暗部有细节**             |

建议下一步行动：

先不要管降噪，集中精力完成 阶段 1。创建一个 SSGI.usf，先试着输出一些调试颜色（比如输出法线、输出深度重建结果），确保你的 Shader 能正确读取 GBuffer 和 HZB，然后再写 Ray Marching 逻辑。