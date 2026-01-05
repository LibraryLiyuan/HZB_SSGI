# HZB_SSGI

基于 **HZB** 的屏幕空间全局光照 **(SSGI)** 渲染

## 实现清单：

- [x] HZB生成
- [x] RayTracing函数，SSGI
- [x] 联合双边滤波单帧降噪
- [x] Temporal累积
- [ ] A-Trous Wavelet 加速单帧降噪
- [ ] DLC：实现HZB_SSR

## 效果展示

> 1SPP, 已经关闭场景里面的Luman GI和Luman Reflection

**效果对比GIF**

<img src="./images/HZB_SSGI.gif" alt="HZB_SSGI" style="zoom:67%;" />

**未开启SSGI**

<img src="./images/image-20260105171642397.png" alt="image-20260105171642397" style="zoom:67%;" />

**SSGI Pass**

<img src="./images/20260105-171707.gif" alt="20260105-171707" style="zoom:67%;" />

**SSGI Pass + Denoise Pass**

<img src="./images/20260105-171750.gif" alt="20260105-171750" style="zoom:67%;" />

## RenderDoc抓帧分析

先看看UE的GPU Visualizer的Pass截图
![image-20260105172210042](./images/image-20260105172210042.png)

<p align="center"><b>UE的GPU Visualizer的抓帧</b></p>

可以看到，HZB_SSGI插入到PostProcessing中，然后先生成了HZB，然后进行了SSGI Trace Pass生成带有噪声的SSGI效果，然后进行了Spatial Denosie对图片进行降噪，然后进行了Temporal通过多帧插值进行平滑噪点，最后进行Composite，颜色混合输出。

### HZB Texture

进行抓帧查看中间结果的图片，先看看HZB Texture Mip0的时候的图片。

<img src="./images/image-20260105172829592.png" alt="image-20260105172829592" style="zoom: 50%;" />

<p align="center"><b>HZB-Mip 0</b></p>

<img src="./images/image-20260105173019962.png" alt="image-20260105173019962" style="zoom:50%;" />

<p align="center"><b>HZB-Mip 4</b></p>

<img src="./images/image-20260105173033652.png" alt="image-20260105173033652" style="zoom:50%;" />

<p align="center"><b>HZB-Mip 7</b></p>

### SSGI Pass

<img src="./images/image-20260105173942906.png" alt="image-20260105173942906" style="zoom: 67%;" />

<p align="center"><b>SSGI Raw Ouput</b></p>

可以看到噪点非常严重。

### Denoise Pass

<img src="./images/image-20260105174004197.png" alt="image-20260105174004197" style="zoom: 67%;" />

<p align="center"><b>SSGI Denoise Ouput</b></p>

### Temporal Pass

<img src="./images/image-20260105174056455.png" alt="image-20260105174056455" style="zoom:67%;" />

<p align="center"><b>SSGI Temporal Ouput</b></p>
