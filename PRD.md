# Ruzino PRD 

- [x] Rebase current agent-dev branch onto dev branch. Resolve any merge conflicts gracefully by keeping the best changes from both branches. After resolving, run tests to ensure nothing is broken.

- [x] 现在有两处AddLibrary.cmake，有一个在rznode里面，而rznode是个submodule，我希望呢，不要有两份重复代码，不然容易维护时出问题，但是又希望rznode能够独立运行和构建，怎么做比较好呢？帮我弄一个优雅的方案。
- [x] AddLibrary.cmake那两处还是不是很干净，那几个flag一直存在有什么问题吗？就始终存在，然后只保留一份AddLibrary就行，保留rznodes里面的就行
- [x] 之前有调整过，要求现在的cmake install能够把整个项目安装出来，然后现在dll被install出来了，但是exe没有被安装出来，我需要你至少把application install出来，可选地把test install出来，然后把依赖的其他dll、资源 (观察现在的configure.py，可以修改利用它) 也要放到install的正确目的地去。并且让run_all_tests在那个install的目的地也成功。现在本目录下有个RuzinoInstall，你需要搞到../RuzinoInstall去做测试。



- [x] 看了一下你的上一次完成，挺好的了。但是现在无法启动，一眼看过去就是因为缺少usd的那些dll，当然别的dll也缺。我希望你创建一个构建and安装的脚本，参数的话可以接受一个安装的目录，以及是否包含测试。如果选择了包含测试就还要运行测试。整体逻辑上，就是把现在安装到Binaries/Release的逻辑给迁移到安装到我指定的那个目录，然后安装后的结果既能独立运行并且通过测试，又能够使用其他项目find_package到，像一个sdk一样工作。

- [x] 运行测试的时候，会报出大量的 找不到 spdlog.dll, glfw3.dll， imgui.dll，nanobind.dll，应该还是cmake install的步骤有问题。尝试修复。
