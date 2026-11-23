# 声场旋转 (Pann) · 声核效果器

基于复平面旋转变换 $f(z) = z \cdot e^{i\omega}$（其中 $z = L + iR$）的 Windows 声核（Audcore）声场旋转效果器：

$$\text{out}_L = L\cos\omega - R\sin\omega, \quad \text{out}_R = R\cos\omega + L\sin\omega$$

- **零延迟、能量守恒**：纯正交复平面旋转，内置 5 ms 线性角度平滑，无过采样相位失真。
- **45° 旋转李萨如散点相位仪**：实时显示变换后声场散点衰减轨迹（单声道 `M` 垂直向上，`-45°` 左声道 `L` 指向左上，`+45°` 右声道 `R` 指向右上），支持一键「收起 / 展开」声场相位图并在收起后彻底停止采样与绘图计算。
- **1° 步进滑块与精确整数输入**：支持 `-180°` 至 `+180°` 1° 步进拖动、双击滑块归零（`0°`）、滚轮微调以及整数文本框直接输入。

## 构建与验证

需要 Visual Studio C++ 工具链、Windows SDK、CMake (>= 3.22) 和 Python 3。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-local-pann.ps1
```

构建产物位于 `build/package/pann-v0.1.3.fx.audcore`。

## 许可证

本项目采用 [GNU General Public License v3.0 (GPLv3)](LICENSE) 开源许可。
