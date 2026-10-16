#简述
	基于C++/Qt 的OpenGL/Vulkan双后端模型浏览器
	可加载STL OBJ GLB OSGB
	支持点选 高亮 隐藏 移动 旋转 缩放 撤销重做
	OSGB通过OSG 读取，使用自写后端渲染，支持视锥剔除 LOD 后台分页加载 缓存回收
	调试器提供渲染选项 向上轴 LOD 范围 层级着色 瓦片外框 内存 帧时间统计

#架构
	[入口]
	main.cpp
		QApplication与QCommandLineParser初始化，解析模型路径 --info --vulkan
		--info调用MeshLoader静态接口，读取并打印模型统计，不创建主窗口
		GUI创建MainWindow，启动Qt事件循环

	[界面层]
	主窗口 MainWindow : QMainWindow
		构造
			connect loadFinished → updateBackendUi
		工具栏 文件打开 后端切换 场景清空 日夜主题
		持有GLWindow或VKWindow，通过Scene接口操作当前场景
		连接加载进度 选择状态 撤销重做状态等信号，更新界面

	调试窗口 DebugWindow : QWidget
		通过Scene修改渲染选项 相机设置 LOD范围 缓存策略
		定时读取debugSnapshot，显示帧时间 资源和进程内存统计
		注册DebugSnapshot为qt元类型，GLWindow 初始化时注册，供调试信息传给调试窗口
		
	帧时间图 FrameGraph : QWidget
		定义在debugwindow.cpp，使用QPainter绘制帧时间曲线

	[模型与读取]
	顶点 Vertex
		交错布局：位置xyz 法线xyz UV，共8个float
	子网格 SubMesh
		顶点 索引 包围盒 材质参数 基础色/金属粗糙度/法线贴图来源
		瓦片归属 层级 revision及resident/uploaded/drawable状态
	网格 Mesh
		子网格集合 整体包围盒 顶点/三角形统计 可选TileNode根节点
		releaseGeometry释放CPU顶点和索引，保留材质 包围盒及统计
		跨线程注册的是std::shared_ptr<Mesh>元类型，不是按值传递Mesh

	模型读取 MeshLoader : QObject
		静态load(path, mesh, error, progress)：按扩展名分派，供CLI及内部调用
		实例load(paths)：移入QThread后由队列调用
			make_shared<Mesh>()创建结果，解析完成发送finished(ok, error, shared_ptr<Mesh>)
			后台只读取和转换CPU数据，不调用OpenGL/Vulkan
		STL
			按文件长度和三角形数判定二进制，否则解析ASCII
			生成三角网格，合并顶点并处理法线
		OBJ
			读取位置 法线 UV，解析面索引及负索引，三角化多边形
			按位置/UV/法线索引组合构造顶点，按材质拆子网格
			读取MTL基础色及贴图路径，缺法线时累加三角形叉积并归一化
		GLB：loadGlb
			校验文件头标识 版本 长度，提取JSON块和BIN块
			通过accessor/bufferView的偏移 步长 类型 数量读取位置 法线 UV和索引
			遍历场景节点，累积父变换×节点matrix或TRS
			节点变换烘焙进顶点，法线用逆转置变换，缺法线时相邻三角形的叉积累加并归一化
			没有索引则生成顺序索引
			读取基础色 金属度 粗糙度及三类贴图来源，支持内嵌图片和外部路径
		普通格式解析后汇总整体包围盒

	OSGB读取 OsgbLoader
		OSG只负责读取，不使用osgViewer，由本项目后端绘制
		转换场景节点 几何 材质和纹理，建立TileNode树
		支持多入口 外部分页 局部原点 小几何合并
		readPage用于后台读取外部页，检查取消标志并返回独立分页数据

	[状态层]
	场景 Scene
		持有Mesh CameraController SelectionController EditController RenderOptions TileStream
		统一加载状态 对象可见性 选择 变换 撤销重做 相机设置及帧率设置
		操作先修改CPU状态并通知界面，再标记变化并请求重绘

		公共加载流程：beginLoad → acceptLoadResult → finishUpload
			窗口loadMeshAsync创建后台MeshLoader及QThread，建立信号连接
			beginLoad记录路径，Idle → Reading，通知加载开始和0%进度
			队列调用MeshLoader::load；reportLoadProgress把解析进度映射到0～90%
			MeshLoader::finished → 窗口lambda → Scene::acceptLoadResult
			检查结果及后端可用性，Reading → Uploading
			setMesh(std::move(*mesh), path)，移动几何数据进入场景
			初始化包围盒 选择中心 编辑状态 相机取景，设置资源变化标记
			通知模型统计和90%进度，请求重绘；此时尚未上传GPU
			后端在绘制阶段上传，调用finishUpload，Uploading → Idle
			通知100%进度和loadFinished，窗口钩子将通知转换为Qt信号
			失败返回Idle并通知；当前没有onLoadFinished函数

		清空与旧加载结果
			读取期间clear进入Discarding，清空场景并结束界面加载提示
			旧结果返回时丢弃，Discarding → Idle，不让旧模型重新出现
			Discarding期间beginLoad拒绝新任务，等旧Worker返回后才能重新加载
			这是丢弃结果，不是中断正在进行的文件解析

	变化记录 SceneChanges
		mesh textures drawOptions grid gizmo tileBounds及objects对象集合
		双后端在绘制前消费标记，同帧重复修改合并，对象集合取出后同步
		普通模型资源更新用变化标记；瓦片槽位复用仍用SubMesh::revision区分资源身份

	渲染选项 RenderOptions
		深度 背面剔除 线框 MSAA Mipmap PBR 法线贴图 调试显示
		网格 操作器 选择描边 坐标轴HUD OSGB按需重绘和固定帧率

	相机 Camera
		轨道视角 位置和目标 透视/正交投影 Y/Z向上 旋转 平移 缩放
	相机控制 CameraController
		持有Camera，根据模型包围球复位取景
	相机矩阵 RenderFrame
		按相机和视口生成共享view/projection
		makeGizmoMatrix生成固定128逻辑像素的操作器矩阵
		makeAxesHudMatrix生成角落坐标轴HUD矩阵
	向上轴 UpAxis / 后端类型 RenderBackend
		分别描述Y/Z向上及OpenGL/Vulkan后端

	选中状态 SelectionController
		保存选中索引和各对象选择中心，实际拾取由后端窗口/渲染器执行
	变换数据 EditTransform
		位置 四元数旋转 缩放
		matrix(center) = T(center+position) × R × S × T(-center)
	变换与撤销 EditController
		普通子网格保存独立变换，OSGB按owner共享变换，取消选中后保留
		保存拖动前后状态，提交命令，管理撤销/重做及无效操作判断
		共用轴方向 角度跨界 旋转 缩放等计算；屏幕射线和输入处理留在窗口

	[渲染层]
	通用绘制 Renderer
		复用子网格可见性 可绘制状态 变换 瓦片归属及材质稳定排序
		acceptsDraw筛选可见/就绪资源及选中对象范围

	GLWindow : QOpenGLWindow, Scene
		异步加载 loadMeshAsync
			connect MeshLoader::finished → acceptLoadResult
			QMetaObject 异步调用 MeshLoader::load
			主线程接管模型指针 setMesh(move(*mesh))
		加载完成 acceptLoadResult
			主线程接管模型指针 setMesh(move(*mesh))
			标记模型待上传，绘制前提交GPU资源
				串行上传子网格 for (const auto& s : mesh.subMeshes) append(s);
					vbo glGenBuffers glBindBuffer
					vao glGenVertexArrays glBindVertexArray
					顶点属性指针 glVertexAttribPointer glEnableVertexAttribArray
					上传顶点 glBufferData
			清理cpu m_mesh.releaseGeometry();
			发上传完成信号 emit loadFinished
			update();
			
		initializeGL初始化函数表 着色器 GLRenderer和辅助绘制资源
		loadMeshAsync负责线程连接与请求派发，结果交给Scene公共流程
		paintGL在当前上下文中同步资源，计算矩阵并绘制模型及辅助内容
		syncResources消费SceneChanges
			mesh：清旧资源 → 普通模型upload → 释放CPU几何 → finishUpload
			textures：同步Mipmap；objects：同步变换/可见性；grid/gizmo：更新辅助几何
		鼠标键盘处理相机和编辑；离屏颜色ID拾取，处理DPI和Y翻转
		选择描边使用模板测试及裁剪空间像素外扩
		析构时makeCurrent，释放GPU资源，再doneCurrent并销毁函数表

	GLRenderer : Renderer
		Mesh整体上传，循环上传子网格 for (const auto& s : mesh.subMeshes) append(s);，每子网格独立vao/vbo等
			vbo glGenBuffers glBindBuffer
			vao glGenVertexArrays glBindVertexArray
			顶点属性指针 glVertexAttribPointer glEnableVertexAttribArray
			上传顶点 glBufferDataglEnableVertexAttribArray记录顶点属性
		瓦片分帧增量上传，完成后Scene::markUploaded，释放对应CPU几何
		按revision处理回收和槽位复用，共享纹理按引用释放，最后一个引用消失才删除
		render绑定子网格材质 纹理 模型矩阵，调用glDrawElements

	着色器 Shader
		读取GLSL 编译链接程序，管理bind及uniform设置
		model.glsl处理坐标变换 普通光照/PBR 法线贴图及像素描边
		overlay.glsl绘制网格 操作器 坐标轴等辅助内容
	OpenGL函数表 GLFunctions : QOpenGLFunctions_4_5_Core
		单例封装函数入口，在有效上下文中初始化，资源释放后显式销毁

	VKWindow : QVulkanWindow, Scene
		loadMeshAsync
			connect MeshLoader::finished → acceptLoadResult
		加载完成 acceptLoadResult
			主线程接管模型指针并放进场景 setMesh(move(*mesh))
			标记模型有更新 SceneChanges.mesh
			请求重绘 requestUpdate();
		重写绘制下一帧函数 createRenderer() override
			返回自定义渲染器 VKRenderer
			
		设置QVulkanInstance及采样数，处理输入和Qt通知
		loadMeshAsync派发后台读取，结果交给Scene公共流程
		createRenderer返回VKRenderer，真正逐帧绘制入口是startNextFrame
		requestUpdate请求新帧，Qt管理设备 交换链 当前命令缓冲和呈现

	VKRenderer : QVulkanWindowRenderer, Renderer
		开始下一帧 startNextFrame() override
			比较待上传 已上传 更新上传 createMeshBuffers()
				destroyBuffer
					vkDestroyBuffer
					vkFreeMemory
				createBuffer
					创建缓冲区对象 vkCreateBuffer
					查询内存要求 vkGetBufferMemoryRequirements
					分配 vkAllocateMemory
					绑定缓冲区到内存 vkBindBufferMemory
					映射内存到cpu地址空间 vkMapMemory
					解除映射 vkUnmapMemory
				清空cpu顶点/索引 Mesh::releaseGeometry()
				发上传完成信号 finishUpload()
					emit loadFinished();
			开始渲染通道 vkCmdBeginRenderPass
			设置视口 vkCmdSetViewport 映射到帧缓冲的矩形
			设置裁剪矩形 vkCmdSetScissor 实际写入像素区域
			绑定图形/计算管线 vkCmdBindPipeline
			结束渲染通道 vkCmdEndRenderPass
			提交当前帧 m_window->frameReady();

		initResources创建描述符布局 着色器模块及设备级资源
		initSwapChainResources创建管线 拾取及单采样目标
		releaseSwapChainResources/releaseResources按Qt回调释放相应资源

		startNextFrame：同步资源 → 更新参数 → 录制绘制 → frameReady
			createMeshBuffers消费changes.mesh，等待旧GPU工作完成后替换资源
				普通模型合并顶点/索引，每个SubMesh记录firstIndex vertexOffset indexCount
				createBuffer：创建 → 查询要求 → 分配内存 → 绑定 → 映射复制 → 解除映射
				几何采用HOST_VISIBLE | HOST_COHERENT内存直接写入
				读取三类材质图片，uploadTexture建立纹理
					CPU生成Mipmap → staging buffer → device-local VkImage
					布局UNDEFINED → TRANSFER_DST → SHADER_READ_ONLY，提交并等待复制完成
					创建ImageView和Sampler，共享图片按缓存键复用GPU纹理
				createMeshDescriptors为子网格绑定动态UBO和三类贴图，缺图用白纹理兜底
				释放CPU顶点/索引，finishUpload结束加载流程
			同步纹理 绘制配置 网格及瓦片资源变化，部分上传失败保留标记请求重试
			每帧更新矩阵 材质及开关，UBO按交换链图像隔离，动态offset选择子网格
				MVP = clipCorrectionMatrix × projection × view × objectTransform
				UBO间距按minUniformBufferOffsetAlignment对齐
			vkCmdBeginRenderPass清颜色/深度，设置viewport及scissor
			绑定图形管线 顶点/索引缓冲 描述符和push constants
			按可见性和绘制顺序调用vkCmdDrawIndexed
			绘制选择描边 蒙版 操作器和坐标轴，vkCmdEndRenderPass
			frameReady通知录制完成，由Qt处理后续提交和呈现
		Vulkan模型着色器
			顶点阶段变换位置，用modelView逆转置变换法线
			片段阶段采样基础色 金属粗糙度 法线贴图，按开关选择普通光照或PBR
			法线贴图用屏幕导数构建切线基；金属粗糙度取G粗糙度 B金属度
		拾取使用离屏R32_UINT对象ID及深度，回读结果；不可用时走窗口备用拾取
		资源替换采用保守同步，当前纹理上传仍等待队列空闲

	[调试与统计]
	帧统计 DebugSnapshot
		FPS DrawCall 三角形数 帧时间曲线 平均/P95/P99及相机和后端信息
		声明为Qt元类型；DebugWindow通过Scene::debugSnapshot读取当前后端快照
	资源统计 DebugResourceInfo
		资源类型 数量 CPU字节 GPU字节及是否可查询
	进程内存 ProcessMemory / processMemory()
		查询工作集 专用工作集 专用提交量，调试窗口低频采样
		系统实测值与瓦片缓存估算口径不同，不能直接相加
		顶点内存：`vertices.capacity() × sizeof(Vertex)`
		索引内存：`indices.capacity() × sizeof(unsigned)`
		GPU 几何估算：已保存的顶点数、三角形数乘以对应字节数
		图片内存：读取图片字节数，并通过集合去重
		节点内存：累加节点及其子节点、槽位等容器的容量

	[瓦片管理]
	层级树 TileNode
		保存路径 包围球 LOD范围 源变换 局部原点 继承材质 子节点和渲染槽位
		loaded/loading/indexed等状态区分资源与索引；回收后保留已发现层级

	调度/回收/统计 TileStream
		主线程修改场景，后台任务仅返回独立结果
		缓存瓦片代表索引 外框线段及外框修订号
		层级选择
			视锥剔除，按距离或投影尺寸匹配LOD范围
			目标层不就绪时requestOnly请求资源，继续绘制可用回退层
			ready检查GPU上传状态；普通Group要求子分支全部就绪，LOD允许可用替代分支
			通常完整细层就绪后替换粗层；LOD范围重叠时仍可能同时绘制，未强制互斥
		后台调度
			每场景最多4个任务，优先屏幕中心，同优先级偏向大瓦片
			每帧重建待提交队列，视野外任务协作取消
			OSG读取不能强制打断，在读取结束和转换检查点响应取消
			主线程合入结果，分配或复用SubMesh槽位并更新revision
			后端分帧上传后markUploaded，触发重新选择LOD
		缓存回收
			默认1024MiB软预算，默认关闭回收，启用后超过预算才回收
			优先最深闲置页，同层按最久未使用，保护当前访问 加载中 选中页及活动祖先
			释放页内几何/图片引用，外部子页独立回收，槽位复用
			revision通知后端替换旧资源；保留索引以便直接重载细层
		内存统计
			累计容器容量 已保存几何数量 图片字节数，图片按缓存键去重
			CPU缓存 GPU估算 后台读取/转换分开显示，预算只计CPU与GPU
			不逐点逐面扫描，但会遍历子网格和层级树，回收前后及调度末尾重新统计
	层级设置 TileLevelSettings
		记录层级开关 默认/覆盖LOD范围及距离/像素模式
	缓存统计 TileStats
		驻留 可见 加载 排队 失败数量及CPU/GPU/后台字节数
	后台内存 PageMemory
		原子记录读取状态 源数据及转换后数据字节数

#环境
	vcpkg install qt6-base qt6-tools gtest
	apt install -y build-essential cmake ninja-build qt6-base-dev libgl1-mesa-dev libglu1-mesa-dev libxkbcommon-x11-dev libgtest-dev fonts-wqy-microhei fonts-noto-cjk

#命令行
	只测试
	gl3d --info path
	build_u24/bin/gl3d --info "data/春季花卉/春季花卉.obj" 2>&1; echo "exit=$?"
	wsl运行
	export GALLIUM_DRIVER=d3d12
	./gl3D

#CI/CD
	冒烟测试：自动测试基本功能

#TODO
	支持透明纹理
	projMatrix固定深度比改成对数，拉近更精细
	扩展.gltf实现顶点去重 索引重排，提高缓存命中 压缩vertex，加速加载
	无xcb/wayland无头可能QApplication 构造直接 abort，--info 根本没机会执行
	文件压缩 Draco meshopt 减少读取体积
	纹理压缩 KTX2/Basi 减少显存
	超大纹理上限/降分辨率/流式加载

#osg场景管理
	视锥剔除 LOD/PagedLOD
	DatabasePager                  后台加载外部节点 按优先级调度，并合入场景
	分页节点过期 回收
	gl增量编译
	渲染状态排序 复用
	osgUtil::Optimizer 按需合并几何
