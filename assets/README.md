# 应用图标

使用内置 image_gen 工具生成透明 PNG；仅进行多尺寸 ICO 格式转换，未改变设计。

Prompt: Create a polished Windows desktop application icon for a professional multi-channel network video recorder. Single centered bold symbol: a dark navy blue geometric video monitor frame with square corners, containing one large vivid red recording circle, with two simple blue signal bars beneath, coherent compact silhouette. Flat vector-like graphic, precise clean edges, restrained broadcast engineering aesthetic, legible at 16 and 32 pixels. No text, no letters, no watermark, no gradients, no shadow, no mockup, no surrounding tile. Actual transparent background. Square canvas, icon occupies 88 percent of canvas with balanced padding. Use case: logo-brand. Asset: production application icon.

app-icon.png 为原始生成文件；app-icon.ico 包含 16、20、24、32、40、48、64、96、128、256 像素图层。CMake 通过 app.rc 将其嵌入 EXE。

最终修改提示：保留透明背景与深蓝显示器轮廓，将红色录制圆点缩小并上移，在屏幕内下方增加粗体无衬线 NDI 字样，保持小尺寸可辨识。使用内置 image_gen 编辑。

深色任务栏修订（2026-10-01）：使用内置 image_gen 编辑，增加不透明浅色方形底板及灰色边框，保留深蓝显示器、NDI 字样和红色录制圆点，加大字样以改善小尺寸识别。仅底板外部透明。
