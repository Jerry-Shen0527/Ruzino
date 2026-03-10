# Ruzino PRD 

- [x] Rebase current agent-dev branch onto dev branch. Resolve any merge conflicts gracefully by keeping the best changes from both branches. After resolving, run tests to ensure nothing is broken.

- [x] 现在有两处AddLibrary.cmake，有一个在rznode里面，而rznode是个submodule，我希望呢，不要有两份重复代码，不然容易维护时出问题，但是又希望rznode能够独立运行和构建，怎么做比较好呢？帮我弄一个优雅的方案。
- [x] AddLibrary.cmake那两处还是不是很干净，那几个flag一直存在有什么问题吗？就始终存在，然后只保留一份AddLibrary就行，保留rznodes里面的就行
- [ ] 之前有调整过，要求现在的cmake install能够把整个项目安装出来，然后现在dll被install出来了，但是exe没有被安装出来，我需要你至少把application install出来，可选地把test install出来，然后把依赖的其他dll、资源 (观察现在的configure.py，可以修改利用它) 也要放到install的正确目的地去。并且让run_all_tests在那个install的目的地也成功。现在本目录下有个RuzinoInstall，你搞到../RuzinoInstall去做测试。


