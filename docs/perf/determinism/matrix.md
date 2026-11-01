# Determinism matrix

Every example at tick 240 must print the golden state hash on every row.

| Host | CPU | ISA | GPU / device | Backends | Variants | Examples | Result |
|---|---|---|---|---|---|---|---|
| Edith-Arc | Intel(R) Core(TM) Ultra 9 185H | sse2 sse3 ssse3 sse4_1 sse4_2 avx avx2 | 3.3.0 - Build 32.0.101.8132 (Intel(R) Arc(TM) Graphics), 4x MSAA, compatible tier; Vulkan 1.4.325 (Intel(R) Arc(TM) Graphics), compatible tier | gl, vulkan | 3 | 10 | 60/60 match |
| Edith-RTX | Intel(R) Core(TM) Ultra 9 185H | sse2 sse3 ssse3 sse4_1 sse4_2 avx avx2 | 3.3.0 NVIDIA 610.88 (NVIDIA GeForce RTX 4070 Laptop GPU/PCIe/SSE2), 4x MSAA, compatible tier; Vulkan 1.4.341 (NVIDIA GeForce RTX 4070 Laptop GPU), compatible tier | gl, vulkan | 3 | 10 | 60/60 match |
| Jarvis_AMD-Ryzen-7-5800X3D-8-Core-Processor | AMD Ryzen 7 5800X3D 8-Core Processor | sse2 sse4_2 avx avx2 fma bmi2 | 3.3.0 NVIDIA 615.78.08 (NVIDIA GeForce RTX 3060 Ti/PCIe/SSE2), 4x MSAA, compatible tier; Vulkan 1.4.351 (NVIDIA GeForce RTX 3060 Ti), compatible tier | gl, vulkan | 3 | 10 | 60/60 match |
| Jarvis_AMD-Ryzen-7-5800X3D-8-Core-Processor-chrome | AMD Ryzen 7 5800X3D 8-Core Processor | sse2 sse4_2 avx avx2 fma bmi2 | OpenGL ES 3.0 (WebGL 2.0 (OpenGL ES 3.0 Chromium)) (WebKit WebGL), 4x MSAA, compatible tier; nvidia ampere (WebGPU (browser)), 4x MSAA, presenting to the window | webgl2, webgpu | 1 | 10 | 20/20 match |
| Jarvis_AMD-Ryzen-7-5800X3D-8-Core-Processor-firefox | AMD Ryzen 7 5800X3D 8-Core Processor | sse2 sse4_2 avx avx2 fma bmi2 | OpenGL ES 3.0 (WebGL 2.0) (NVIDIA GeForce GTX 980, or similar), 4x MSAA, compatible tier | webgl2 | 1 | 10 | 10/10 match |
| Tadashi_AMD-A6-5200-APU-with-Radeon-TM-HD-Graphics | AMD A6-5200 APU with Radeon(TM) HD Graphics | sse2 sse4_2 avx | 4.5 (Core Profile) Mesa 26.0.8-1ubuntu0.3 ( (radeonsi, kabini, ACO, DRM 2.51, 7.0.0-22-generic)), 4x MSAA, compatible tier; Vulkan 1.3.335 (AMD Radeon HD 8400 / R3 Series (RADV KABINI)), compatible tier | gl, vulkan | 3 | 10 | 60/60 match |
