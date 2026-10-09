# Third-party notices / 他者のソフトウェアの表示

This repository contains code derived from, or files taken from, the projects below.
このリポジトリには、下記のプロジェクトから移植したコード、またはそのままのファイルが含まれます。

Models are not included. Video Depth Anything Small is Apache-2.0; **Video Depth Anything Large is CC-BY-NC-4.0 (non-commercial)** and is used only by Depth Engine = Remote Quality.
モデルは含めていません。Video Depth Anything Small は Apache-2.0、**Large は CC-BY-NC-4.0（非商用）** で、Depth Engine = Remote Quality のときだけ使います。

The After Effects SDK is not included. / After Effects SDK は含めていません。

---

## TypeGPU

The lighting model in `plugin/src/RelightCore.h`, `plugin/src/RelightGPU.cu` and `plugin/src/RelightAE.cpp` is ported from the TypeGPU example "Monocular Light Injection".
照明モデルは TypeGPU の作例「Monocular Light Injection」を移植したものです。

https://github.com/software-mansion/TypeGPU

```
MIT License

Copyright (c) 2025 Software Mansion

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## ONNX Runtime

The headers in `third_party/onnxruntime/include/` are from ONNX Runtime.
`third_party/onnxruntime/include/` のヘッダは ONNX Runtime のものです。

https://github.com/microsoft/onnxruntime

```
MIT License

Copyright (c) Microsoft Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
