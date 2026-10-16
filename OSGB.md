# OSGB 分页浏览

默认启用 `GL3D_ENABLE_OSGB=ON`。OSG 负责文件解析和图片解码，不使用 osgViewer；转换后的几何由现有 OpenGL 或 Vulkan 后端渲染。

## 打开数据

运行 `build/bin/Release/gl3d.exe`，通过工具栏选择后端，再打开场景入口 `.osgb`。保留入口文件、子瓦片和纹理的相对目录。只有单个叶子文件时，只显示该叶子的内容。不读取 XML 地理坐标元数据。

```powershell
.\build\bin\Release\gl3d.exe 'D:/dataset/root.osgb'
.\build\bin\Release\gl3d.exe --vulkan 'D:/dataset/root.osgb'
```

`--vulkan` 启动独立 Vulkan 窗口；需要调试面板时使用主窗口的后端切换按钮。`--info` 统计入口文件内已解析的几何，不递归加载全部外部页。

## 已实现的行为

调试器的 OSGB 栏仅在 OSGB 场景中显示。按需模式默认开启：当前视野的分页读取和 GPU 上传完成后停止自动重绘，操作相机、编辑或修改调试选项后恢复更新。关闭按需模式后持续重绘。固定 FPS 只控制活动期间的更新频率，按需模式下静止时仍停止绘制。

- 读取三角网格、变换、基础漫反射材质及第 0 组纹理，包括嵌入纹理和 DDS DXT1/3/5。外部页继承父节点基础材质。以双精度烘焙源变换后减去场景原点，降低大坐标精度损失。
- 两个后端共享视锥剔除、距离/像素 LOD、PagedLOD 外部页解析、屏幕中心优先级调度和后台加载。细节页完成 GPU 上传前保留可用粗层；读取失败重试并保留粗层，检测外部引用环。
- 默认最多 4 个后台页任务、1024 MiB 估算缓存预算，默认关闭回收，无时间过期，参数在 `TileStream` 中。可见页、正在加载的页、选中瓦片和入口常驻几何受到保护，预算是软限制。
- OpenGL 几何按帧增量上传，目标每帧 4 MiB / 4 ms；Vulkan 每帧限制新增上传单元数和时间。单张纹理分配/上传不能保证遵守时间上限；Vulkan 资源替换和纹理上传仍采用保守同步，可能产生瞬时卡顿。
- 复用稳定渲染槽位与已有 GPU 资源，共享同页的相同纹理，缓存按纹理排序的绘制顺序；同节点同材质的小几何按需合并，不跨瓦片编辑边界合并。
- 通过现有拾取及编辑工具移动、旋转、缩放整瓦片，后续加载的 LOD 继承相同变换和显隐状态，支持撤销/重做。编辑仅作用于本次浏览，不写回 OSGB。
- 调试面板保留线框、法线/UV、深度、背面剔除、MSAA（受后端能力约束）、Mipmaps、轮廓、FPS 与资源统计。OSGB 未映射 PBR 和法线贴图，对应选项置灰。分页统计的“瓦片单元”是渲染几何单元数，不等于外部文件数。

## 兼容范围与测试

面向常见摄影测量 OSGB 网格。任意 OSG 自定义着色器、顶点颜色、透明混合、特殊 Drawable、动画和完整 StateSet 语义不保证复现，遇到不支持的内容会输出警告。已有 LOD/PagedLOD 才能流式细化；单体大 OSGB 不会自动生成金字塔。

自动化测试使用生成的 OSGB 数据，覆盖纹理/材质、跨页继承、大坐标、几何合并、剔除、距离/像素 LOD、粗层回退、失效回收与重载、编辑状态、取消、引用环和场景复制。OpenGL 离屏测试验证分帧上传、纹理共享及整瓦片绘制。真实大场景显示、Vulkan 驱动运行行为和目标机器性能需要实际文件验证。

```powershell
cmake --build build --config Release --target gl3d gl3d_test gl3d_gl_stream_test --parallel 6
ctest --test-dir build -C Release --output-on-failure
```

构建会部署所需 OSG 运行库和 `osgPlugins-3.6.5` 下的常用读取插件，不部署 osgViewer。移动程序时应复制整个输出目录。

## 多选 OSGB

打开模型时可用 Ctrl 或 Shift 多选 `.osgb`，全部作为同一个场景加载。各文件保留原始相对位置、独立瓦片编辑归属和各自的分页相对路径；相机按整体范围取景。多选只支持 OSGB，不支持与 OBJ/STL/GLB 混选。文件应使用同一坐标系，程序不进行坐标系转换，也不自动拼接局部坐标数据。

## 向上轴

调试器的“调试视图”中可选择 Y/Z 向上轴。每次加载 OSGB（含多选）默认 Z，其他格式默认 Y。Z 向上对应 XY 地面网格，Y 向上对应 XZ 地面网格。切换时重新设置相机观察方向，保留模型位置和当前取景距离，不旋转模型数据。Z 向上时鼠标平移、W/S/A/D 沿地面移动，Q/E 沿高度轴移动。

## 生成目录入口

项目增加 `gl3d_osgb_index` 命令行工具，扫描实际 PagedLOD/ProxyNode 外部引用关系，找出未被其他文件引用的有效入口，跳过空文件，并输出引用检查报告。保留源文件内的 LOD 范围；它不为没有 LOD 的独立网格自动生成简化金字塔。页引用使用相对路径。默认相对于输出目录；`--same-directory` 按源目录生成，适合先在工作区生成再复制到瓦片目录。入口与瓦片一起移动时无需重新生成。

```powershell
.\build\bin\Release\gl3d_osgb_index.exe 'D:/dataset/osgb' 'C:/code/gl3D/data/root.osgb' --same-directory
```

工具拒绝覆盖已有输出；检测到循环引用、无法读取文件或未覆盖的有效节点时停止生成。缺失的外部页列在输出同名 `.json` 报告中，浏览时使用已有粗层回退。

台北数据已生成 `data/taipei-root.osgb`，需复制到原始瓦片同一目录使用。源目录含 2,804 个文件，其中 2,791 个有效文件由 `3143415263404252.osgb` 入口连接，13 个为空节点。缺失细节页为 `314341526340425266116.osgb`。打开生成的入口或原始 `3143415263404252.osgb` 即可，不要再同时多选下级文件。

## 调试器金字塔层级

按 LOD 分支和外部页引用识别层级，L0 最粗，更细层随分页加载逐步显示。每层可勾选显示或隐藏，隐藏中间层不阻断更细层的访问路径；为找到更细页，隐藏的中间页仍可能被读取。距离范围为 [最近, 最远)，使用模型单位；像素 LOD 显示 px，最远 1e30 表示无限。未修改时严格使用每个节点的原始范围，表格显示同层已发现范围的汇总。编辑后该层统一使用自定义范围，二分支 LOD 的粗层共享切换边界同步调整。点击恢复按钮可恢复原始范围并显示所有层。设置只对当前场景有效，不修改 OSGB 文件。

分页回收使用帧编号保护当前视野需要的页及其祖先，包括已读取但仍在增量 GPU 上传的页。不再通过毫秒时间是否相等判断本帧使用，避免预算紧张时细页反复加载后被回收。

层级列表移除单位列，单位规则保留在说明中。可勾选按层级叠加半透明颜色，L0 浅绿、L5 浅红，中间经浅黄渐变，混合强度 45%。共享 Scene 提供层级色和开关，两个后端使用相同的颜色参数。颜色叠在原材质光照结果上，模型仍保持原有深度遮挡；法线/UV 调试视图和选中描边保留原显示。L5 以上使用同一浅红，色标不随新层发现而改变。

后台分页加载最多同时 4 个任务。待提交队列每帧按当前视野重建，按瓦片中心到屏幕中心的像素距离由近到远排序，同距离时屏幕占比大的页优先。已提交页的变换后包围球离开视锥或所属瓦片被隐藏时，发出独立取消信号；不再安装取消结果，不计为加载失败。OSG 底层文件读取无法立即中断，返回后停止后续几何转换。取消任务实际退出前仍占一个名额，避免快速移动视野时任务无限堆积。半个瓦片仍在视野内时不取消。

## 缓存预算与内存对照

调试器可调整预算（默认 1024 MiB），可勾选超预算时按最久未使用回收（默认关闭）。关闭时访问过的页不自动回收；启用时仅在超预算后优先回收最深的闲置页，同层按最近使用时间排序。已删除原有 10 秒过期规则。

预算计费为 CPU 瓦片数据估算加 GPU 瓦片数据估算，同一 QImage 只计一次。不包含所有驱动、分配器、窗口和后台读取开销，不是进程内存上限。后台分开统计已可遍历的 OSG 数组/图片和转换中数据，正在读取的 OSG 内部分配仍不可精确归属。Windows 进程工作集、私有工作集和私有提交由系统 API 每秒采样，可对照任务管理器相应列；这些进程值与资源估算不能直接相加。


Retained page index and layered eviction:
- Discovered page paths, bounds, LOD ranges and transforms survive resource eviction. Returning to a previously visited region can request matching fine pages without reading evicted ancestors first. Unknown regions still require discovery through their parent files.
- Eviction releases one external page and its inline geometry, keeping separately owned external descendants and their index entries. Deepest idle pages are reclaimed first; pages at the same level use LRU order. L3/L4 previews survive while deeper pages can satisfy the budget, but are not pinned when memory remains over budget.
- Parent reloads reuse retained external branches. Active pages and loading paths remain protected.

Merge integration:
- Tile bounds use shared Scene line geometry for both backends and follow visible geometry, tile transforms and hiding. Buffers refresh only when bounds change.
- The parallel OsgbStream pipeline is superseded by TileStream; the 1024 MiB budget, retained index, layered eviction, multi-selection and editor state remain intact.
