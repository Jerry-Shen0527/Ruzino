# Ruzino PRD 

- [x] Rebase current agent-dev branch onto dev branch. Resolve any merge conflicts gracefully by keeping the best changes from both branches. After resolving, run tests to ensure nothing is broken.

- [x] 现在有两处AddLibrary.cmake，有一个在rznode里面，而rznode是个submodule，我希望呢，不要有两份重复代码，不然容易维护时出问题，但是又希望rznode能够独立运行和构建，怎么做比较好呢？帮我弄一个优雅的方案。
- [x] AddLibrary.cmake那两处还是不是很干净，那几个flag一直存在有什么问题吗？就始终存在，然后只保留一份AddLibrary就行，保留rznodes里面的就行
- [x] 之前有调整过，要求现在的cmake install能够把整个项目安装出来，然后现在dll被install出来了，但是exe没有被安装出来，我需要你至少把application install出来，可选地把test install出来，然后把依赖的其他dll、资源 (观察现在的configure.py，可以修改利用它) 也要放到install的正确目的地去。并且让run_all_tests在那个install的目的地也成功。现在本目录下有个RuzinoInstall，你需要搞到../RuzinoInstall去做测试。



- [x] 看了一下你的上一次完成，挺好的了。但是现在无法启动，一眼看过去就是因为缺少usd的那些dll，当然别的dll也缺。我希望你创建一个构建and安装的脚本，参数的话可以接受一个安装的目录，以及是否包含测试。如果选择了包含测试就还要运行测试。整体逻辑上，就是把现在安装到Binaries/Release的逻辑给迁移到安装到我指定的那个目录，然后安装后的结果既能独立运行并且通过测试，又能够使用其他项目find_package到，像一个sdk一样工作。

- [x] 运行测试的时候，会报出大量的 找不到 spdlog.dll, glfw3.dll， imgui.dll，nanobind.dll，应该还是cmake install的步骤有问题。尝试修复。

- [x] 现在把项目整个Install到../RuzinoInstall 是成功的，但是运行不起来。首先我能想到的第一个问题，就是 C:\Users\Pengfei\WorkSpace\Ruzino\source\Core\RHI\source\shaderCompiler.cpp 这里面不停向上找的逻辑会导致死循环，因为Ruzino.exe(以及其他的exe)现在的工作目录变了，和SDK的相对位置变了。修复这一点，可能既要考虑怎么把需要的文件install过去，又要考虑如何设计这里的查找方式。总之这样找parent的方式很蠢。

- [ ] [867/1181] Building CXX object source\Editor\rzpython\CMakeFiles\rzpython.dir\source\tcp_server.cpp.obj         Please define _WIN32_WINNT or _WIN32_WINDOWS appropriately. For example: - add -D_WIN32_WINNT=0x0601 to the compiler command line; or - add _WIN32_WINNT=0x0601 to your project's Preprocessor Definitions. Assuming _WIN32_WINNT=0x0601 (i.e. Windows 7 target). [872/1181] Building CXX object source\Editor\geometry_...s\CMakeFiles\node_write_usd.dir\node_write_usd.cpp.ob  C:\Users\Pengfei\WorkSpace\Ruzino\source\Editor\geometry_nodes\node_write_usd.cpp(31): warning C4190: 'get_or_create_modifier_layer' 已指定 C 链接，但返回了与 C 不兼容的 'pxrInternal_v0_25_5__pxrReserved__::TfWeakPtr<pxrInternal_v0_25_5__pxrReserved__::SdfLayer>' C:\Users\Pengfei\WorkSpace\Ruzino\SDK\OpenUSD\Debug\include\pxr/base/tf/weakPtr.h(127): note: 参见“pxrInternal_v0_25_5__pxrReserved__::TfWeakPtr<pxrInternal_v0_25_5__pxrReserved__::SdfLayer>”的声明 构建的时候有这两个Warning
