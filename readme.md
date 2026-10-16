#简述
	基于C++/Qt 的OpenGL/Vulkan双后端模型浏览器
	可加载STL OBJ GLB OSGB
	支持点选 高亮 隐藏 移动 旋转 缩放 撤销重做
	OSGB通过OSG 读取，使用自写后端渲染，支持视锥剔除 LOD 后台分页加载 缓存回收
	调试器提供渲染选项 向上轴 LOD 范围 层级着色 瓦片外框 内存 帧时间统计
	
#架构
	[界面层]
	主窗口 MainWindow
		工具栏 后端切换 场景清空
		connect loadFinished → updateBackendUi
		updateBackendUi
			上传模型后更新菜单ui
	调试窗口 DebugWindow
		渲染开关 层级范围 缓存与进程内存
		注册DebugSnapshot为qt元类型，GLWindow 初始化时注册，供调试信息传给调试窗口
		
	帧时间图 FrameGraph

	[状态层]
	场景 Scene
		GLWindow / VKWindow共同基类，持有模型和各控制组件
		普通网格按对象保存变换，OSGB按瓦片归属共享变换
		共用撤销重做 选中隐藏 相机设置 渲染选项 帧率定时
		后端仅同步操作器、管线和纹理资源，发出Qt通知
	渲染选项 RenderOptions
	
	相机 Camera
	相机控制 CameraController
		根据模型包围球自动取景
	相机矩阵 RenderFrame
		操作手柄固定128逻辑像素
		
	选中状态 SelectionController
	变换与撤销 EditController
		双后端共享变换 拖动计算 撤销重做
		各对象保存独立变换，取消选中后保留
		变换数据 EditTransform

	[模型]
	点 子网格 网格结构体 Vertex SubMesh Mesh
		注册Mesh为qt元类型，OpenGL/Vulkan 窗口初始化时注册，供后台加载模型传回主线程
			
	模型读取 MeshLoader
		声明模型加载完成信号 void finished
		load
			STL
			OBJ 读位置 法线 uv mtl路径
			GLB解析
			法线用逆转置变换 缺法线时相邻三角形的叉积累加并归一化
			分配模型指针到堆上 std::make_shared<Mesh>()
			加载完成 finished→onLoadFinished
		
	OSGB读取 OsgbLoader
		OSG读取节点，自行转换几何 材质 纹理
		支持多入口 外部分页 局部原点 小几何合并

	[渲染层]
	通用绘制 Renderer
		子网格可见性、变换、瓦片归属和稳定排序
		不持有显存资源，不调用OpenGL/Vulkan

	GLWindow : QOpenGLWindow, Scene
		异步加载 loadMeshAsync
			connect MeshLoader::finished → onLoadFinished
			QMetaObject 异步调用 MeshLoader::load
			主线程接管模型指针 setMesh(move(*mesh))
		加载完成 onLoadFinished
			主线程接管模型指针 setMesh(move(*mesh))
			绑上下文 makeCurrent()
			上传gpu m_renderer->upload(m_mesh);
				串行上传子网格 for (const auto& s : mesh.subMeshes) append(s);
					vbo glGenBuffers glBindBuffer
					vao glGenVertexArrays glBindVertexArray
					顶点属性指针 glVertexAttribPointer glEnableVertexAttribArray
					上传顶点 glBufferData
			清理cpu m_mesh.releaseGeometry();
			发上传完成信号 emit loadFinished
			update();
			
	GLRenderer
			普通模型整体上传，瓦片分帧增量上传
			几何上传后释放CPU顶点与索引，保留材质图片
			共享纹理 按纹理排序 同步瓦片回收
			共享纹理最后一个引用释放才销毁
		着色器 Shader
		传入模型、观察和投影矩阵
		子网格绑定材质与纹理并执行索引绘制
		顶点坐标变换、裁剪和光栅化
		片段着色器计算颜色
		深度测试写入帧缓冲

	VKWindow : QVulkanWindow, Scene
		loadMeshAsync
			connect MeshLoader::finished → onLoadFinished
		加载完成 onLoadFinished
			主线程接管模型指针并放进场景 setMesh(move(*mesh))
			标记模型有更新 ++m_modelRevision;
			请求重绘 requestUpdate();
		重写绘制下一帧函数 createRenderer() override
			返回自定义渲染器 VKRenderer
			
	VKRenderer : QVulkanWindowRenderer
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
				发上传完成信号 meshUploadFinished()
					emit loadFinished();
			开始渲染通道 vkCmdBeginRenderPass
			设置视口 vkCmdSetViewport 映射到帧缓冲的矩形
			设置裁剪矩形 vkCmdSetScissor 实际写入像素区域
			绑定图形/计算管线 vkCmdBindPipeline
			结束渲染通道 vkCmdEndRenderPass
			提交当前帧 m_window->frameReady();
				

	调试数据
		帧统计 DebugSnapshot
		资源统计 DebugResourceInfo
		进程内存 ProcessMemory
			系统实测工作集与专用提交量，每秒采样
			与瓦片缓存估算口径不同，不能直接相加
			顶点内存：`vertices.capacity() × sizeof(Vertex)`
			索引内存：`indices.capacity() × sizeof(unsigned)`
			GPU 几何估算：已保存的顶点数、三角形数乘以对应字节数
			图片内存：读取图片字节数，并通过集合去重
			节点内存：累加节点及其子节点、槽位等容器的容量

	[瓦片管理]
	层级树 TileNode
		保存路径 包围球 LOD范围 变换 子节点
		回收资源后保留索引，重载细层无需重新读取祖先
		
	调度/回收/统计 TileStream
		缓存瓦片代表索引和外框线段，维护外框修订号
		调度
			视锥剔除，按距离或投影尺寸选择LOD
			如目标层尚未准备好，就遍历它以发起加载请求。此时采用 requestOnly
			安装分页 分配revision资源号
			后台任务读取和转换分页，主线程接收结果，将子网格安装到场景，最多4个任务，优先屏幕中心，距离相同时优先大瓦片
			每帧重建待提交队列，视野外任务协作取消
			后端分帧上传资源，完成后调用 Scene::markUploaded()，更新上传状态并请求重新调
			目标层可用后，调度才允许它参与绘制
			粗层瓦片的细层子瓦片全上传后才替换粗层，防粗细层重叠出现深度冲突/闪烁
		回收
			优先最深闲置页，同层优先最久未使用，尽量保留中间层
			保护当前访问 加载中 选中对象的分页及活动祖先
			释放页内几何与纹理引用，外部子页各自回收，槽位复用
			资源版本通知后端释放显存，层级索引继续保留
		统计
			累计容器容量 已保存几何数量 图片字节数，图片去重
			遍历子网格与层级树，不逐点逐面扫描
			每次分页回收前后及每帧调度末尾重新统计
			CPU缓存 GPU估算 后台转换分开显示；预算只计CPU与GPU
	层级设置 TileLevelSettings
	缓存统计 TileStats
	后台内存 PageMemory
		
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
