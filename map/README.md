# 原创 GIS 军事卫星地图

本目录是一套离线 XYZ 瓦片缓存，与当前 Qt 项目的 `MapTileRenderer` 参数对齐：

- Web Mercator（EPSG:3857）
- XYZ 目录结构：`12/{x}/{y}.png`
- 固定缩放级别：`12`
- 瓦片尺寸：`256 x 256`
- 覆盖范围：`x=3404..3409`，`y=1745..1750`
- 项目原点：北纬 `25.40`、东经 `119.30`
- 默认逻辑战区：`20 km x 15 km`

## 运行

配置和构建时，CMake 会将运行所需的 `12/`、`metadata.json` 和
`tilejson.json` 部署到可执行文件旁的 `map/` 目录。按项目标准方式运行：

```bash
cmake --build build --parallel
./build/appindex
```

也可用 `./map/run.sh` 自动选择已构建的可执行文件。部署或测试其他地图时，
可用 `WARGAME_MAP_DIR=/absolute/path/to/map` 显式覆盖运行目录。

## 内容

- `source/mission-area.png`：生成的原创高分辨率卫星母图
- `preview.png`：六乘六瓦片拼接预览
- `12/`：Qt 直接读取的 36 张 PNG 瓦片
- `metadata.json`：投影、范围、对齐和要素说明
- `tilejson.json`：TileJSON 3.0 描述
- `MANIFEST.sha256`：瓦片和元数据完整性清单
- `prompt.txt`：最终生图提示词
- `tools/build_tiles.py`：可重复的母图切片工具

## 重新切片

```bash
python3 map/tools/build_tiles.py map/source/mission-area.png
(cd map && sha256sum -c MANIFEST.sha256)
```
