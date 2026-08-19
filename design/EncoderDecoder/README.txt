VMF 编解码工具 README

一、项目简介

本项目是一个基于 C++20 和 CMake 的 VMF 消息编解码与校验工具集。工具以 XML 字典为驱动，将 XML 格式的消息内容编码为二进制比特流，也可以将二进制比特流按同一字典解码回 XML。项目同时提供消息合法性校验、XML 对比、自动生成测试消息、路线描述文本转换等辅助工具。

核心程序包括：
- `vmf_encode`：将消息 XML 编码为二进制文件。
- `vmf_decode`：将二进制文件解码为消息 XML。
- `vmf_validate`：校验消息 XML 是否符合结构字典和内容字典。
- `xml_compare`：比较两个 XML 文件的结构和值。
- `tools/gen_cases.py`：根据字典生成正向测试消息。
- `tools/run_generated_tests.py`：批量执行生成消息的验证、编解码和对比流程。
- `tools/route_desc_to_msg.py`：将中文路线描述文本转换为 Land Route 消息 XML。
- `tools/route_msg_to_desc.py`：将 Land Route 消息 XML 转换回路线描述文本。

二、XML 文件说明

1. 消息结构字典：`msgStruct/msg0_1.xml`

该文件描述普通消息的层级结构和字段顺序，是编解码的主要结构依据。文件中包含 `Header`、`Body`、`Group`、`Field`、`DataUnit` 等节点。

常见节点含义如下：
- `Group`：表示一组结构化字段，可以是普通组、可选组或重复组。
- `Field`：表示字段容器，可以是普通字段、可选字段或重复字段。
- `DataUnit`：表示实际参与编解码的数据单元。
- `GPI`：Group Presence Indicator，用于表示可选 `Group` 是否存在。
- `FPI`：Field Presence Indicator，用于表示可选 `Field` 是否存在。
- `GRI`：Group Repeat Indicator，用于表示重复 `Group` 是否还有下一项。
- `FRI`：Field Repeat Indicator，用于表示重复 `Field` 是否还有下一项。

`DataUnit` 通过 `DFI` 和 `DUI` 标识具体数据项，并通过 `bits` 指定位宽。编码和解码时，程序按该结构字典中的 XML 顺序递归处理节点。

2. Land Route 消息结构字典：`msgStruct/msg4_2.xml`

该文件描述 Land Route 类型消息的结构，主要用于路线描述转换流程。其结构中包含多条路线、途经点、报告时间、关键点等信息。路线文本转换工具会根据该字典生成可校验和可编码的 `MessageContent` XML。

3. 内容字典：`dic_content.xml`

该文件保存数据内容约束。它以 `DFI/DUI` 为索引，定义数据项名称、位宽以及允许值范围。

主要用途：
- 与结构字典中的 `DataUnit` 做一致性检查。
- 为编码、解码和校验提供标准字段名称。
- 校验消息中 `DataUnit` 的取值是否在允许范围内。

如果消息 XML 中的 `DataUnit` 带有 `name` 属性，该名称必须与结构字典和内容字典中相同 `DFI/DUI` 对应的名称一致。

4. 消息样例 XML

项目根目录下包含若干消息样例：
- `msg_pass.xml`：合法样例，可用于验证、编码、解码和回归对比。
- `msg_fail.xml`：故意构造的失败样例，用于验证错误检测能力。
- `pos_msg.xml`：合法正向样例，部分 `DataUnit` 省略 `name` 属性，用于验证 `DFI/DUI` 驱动查找能力。
- `msg.xml`：遗留示例消息，可用于本地检查。

消息 XML 的根节点通常为：

```xml
<MessageContent message="...">
  <Header>...</Header>
  <Body>...</Body>
</MessageContent>
```

`DataUnit` 推荐写法：

```xml
<DataUnit DFI="4085" DUI="059" name="NetworkIdNumber">11</DataUnit>
```

其中 `name` 可省略，但 `DFI` 和 `DUI` 必须存在。

5. 路线描述文本：`路线描述.txt`

该文件是面向 Land Route 消息的中文文本输入。转换工具识别以下关键字：
- `路径N`
- `途经点`
- `报告时间`
- `关键点`

工具会将路线、坐标和时间等内容转换为 `msgStruct/msg4_2.xml` 对应的 XML 消息结构。

三、构建命令

1. 配置 CMake：

```bash
cmake -S . -B build
```

2. 编译全部目标：

```bash
cmake --build build -j
```

构建产物位于 `build/` 目录。该目录是生成目录，不应提交到版本库。

四、核心命令说明

1. 消息校验：`vmf_validate`

命令格式：

```bash
./build/vmf_validate <结构字典.xml> <内容字典.xml> <消息.xml>
```

示例：

```bash
./build/vmf_validate msgStruct/msg0_1.xml dic_content.xml msg_pass.xml
```

功能说明：
- 检查结构字典与内容字典中相同 `DFI/DUI` 的位宽是否一致。
- 检查消息 XML 中 `DataUnit` 的 `DFI/DUI/name` 是否与字典匹配。
- 检查 `GPI/FPI` 可选结构语义。
- 检查 `GRI/FRI` 重复结构语义。
- 检查重复结构是否超过字典中的 `max` 限制。
- 检查 `DataUnit` 的值是否满足 `dic_content.xml` 中的允许值约束。

返回结果：
- `Validation PASS`：消息合法。
- `Validation FAIL`：消息不合法，并输出失败原因。

2. XML 编码为二进制：`vmf_encode`

命令格式：

```bash
./build/vmf_encode <结构字典.xml> <内容字典.xml> <消息.xml> <输出.bin>
```

示例：

```bash
./build/vmf_encode msgStruct/msg0_1.xml dic_content.xml msg_pass.xml out.bin
```

功能说明：
- 按结构字典顺序遍历消息 XML。
- 根据 `DFI/DUI` 查找数据项位宽。
- 将每个数据值写入二进制比特流。
- 根据 `GPI/FPI/GRI/FRI` 控制可选结构和重复结构的编码。
- 自动回填 Header 中的 length 字段。

3. 二进制解码为 XML：`vmf_decode`

命令格式：

```bash
./build/vmf_decode <结构字典.xml> <内容字典.xml> <输入.bin> <输出.xml>
```

示例：

```bash
./build/vmf_decode msgStruct/msg0_1.xml dic_content.xml out.bin decoded.xml
```

功能说明：
- 按结构字典顺序从比特流读取字段。
- 根据 `bits` 位宽恢复整数值。
- 根据 `GPI/FPI` 判断可选结构是否展开。
- 根据 `GRI/FRI` 判断重复结构何时结束。
- 解码输出的 `DataUnit` 会包含 `DFI`、`DUI` 和标准 `name`。

4. XML 对比：`xml_compare`

命令格式：

```bash
./build/xml_compare <XML文件A> <XML文件B>
```

示例：

```bash
./build/xml_compare msg_pass.xml decoded.xml
```

功能说明：
- 比较 XML 标签、节点顺序、属性和值。
- `DataUnit` 只比较 `DFI`、`DUI` 和文本值，忽略 `name` 差异。
- `Header` 差异作为 warning，不导致失败。
- 根节点 `MessageContent@case` 差异作为 warning，不导致失败。

退出码：
- `0`：对比通过，或仅存在 warning。
- `2`：存在实际差异。
- `3`：XML 文件加载或解析错误。

五、测试集生成与运行

1. 生成正向测试消息：

```bash
python3 tools/gen_cases.py --dict msgStruct/msg0_1.xml --content-dict dic_content.xml --build-dir build --out-dir generated --seed 20260324 --max-repeat 3 --max-cases 20
```

输出内容：
- `generated/positive/`：生成的正向 XML 样例。
- `generated/cases_manifest.json`：样例清单和覆盖目标信息。

2. 运行生成样例测试：

```bash
python3 tools/run_generated_tests.py --dict msgStruct/msg0_1.xml --content-dict dic_content.xml --cases generated --build-dir build
```

运行流程：
1. 先执行 `vmf_validate`。
2. 校验通过后执行 `vmf_encode`。
3. 再执行 `vmf_decode`。
4. 最后执行 `xml_compare`。

测试报告输出为：

```text
generated/report.json
```

六、路线描述转换流程

1. 中文路线描述转 XML：

```bash
python3 tools/route_desc_to_msg.py 路线描述.txt -o route_message.xml
```

2. XML 转回中文路线描述：

```bash
python3 tools/route_msg_to_desc.py route_message.xml -o 路线描述_out.txt
```

3. 执行完整路线管线：

```bash
bash tools/run_route_pipeline.sh --build-dir build
```

完整管线包括：
- 路线文本转 XML。
- XML 校验。
- XML 编码为二进制。
- 二进制解码为 XML。
- 解码 XML 再校验。
- 原 XML 与解码 XML 对比。
- 解码 XML 转回路线文本。

默认输出目录为：

```text
route_pipeline_out/
```

七、推荐使用流程

普通消息回归建议按以下顺序执行：

```bash
cmake -S . -B build
cmake --build build -j
./build/vmf_validate msgStruct/msg0_1.xml dic_content.xml msg_pass.xml
./build/vmf_encode msgStruct/msg0_1.xml dic_content.xml msg_pass.xml out.bin
./build/vmf_decode msgStruct/msg0_1.xml dic_content.xml out.bin decoded.xml
./build/xml_compare msg_pass.xml decoded.xml
```

生成测试集回归建议按以下顺序执行：

```bash
python3 tools/gen_cases.py --dict msgStruct/msg0_1.xml --content-dict dic_content.xml --build-dir build --out-dir generated --seed 20260324 --max-repeat 3 --max-cases 20
python3 tools/run_generated_tests.py --dict msgStruct/msg0_1.xml --content-dict dic_content.xml --cases generated --build-dir build
```

Land Route 消息建议直接使用：

```bash
bash tools/run_route_pipeline.sh --build-dir build
```

八、注意事项

- 所有工具均依赖结构字典和内容字典共同工作。
- 编码前建议先运行 `vmf_validate`。
- 解码程序按字典和指示位恢复结构，不负责判断业务规则。
- `DFI/DUI` 是数据单元匹配的主键，字段名称只是辅助信息。
- `build/`、`generated/`、`route_pipeline_out/` 为生成目录，不应作为源文件维护。
