## Build
Go to Ruzino/build and run:
```
ninja.exe
```

But if there are only shader changes, rebuilding is not needed.

Adding new source files or test files requires re-running cmake and they will be automatically added to the build. Command is
```
cmake .. -DRUZINO_WITH_CUDA=ON -DCMAKE_BUILD_TYPE=Release -DNVRHI_WITH_NVAPI=ON -G Ninja
```

Added test cpp file will be scanned into <filename>_test.exe automatically.

## Test Instructions
Run the following command in Ruzino/Binaries/Release:
```
.\headless_render.exe -u ..\..\Assets\soft_body_neo.usdc -j ..\..\Assets\render_nodes_save.json -o test_gpu.png -w 400 -h 400 -s 1 -f 2 -n -v
```
f = 2 means render 2 frames, which will trigger the soft body simulation in between.

## Simulation
```
.\rz_simulate.exe -u ..\..\Assets\soft_body_neo.usdc -f 5 -v
```

Or
```
.\Ruzino.exe somestage.usdc
```

If it is some test, just use the cpp name plus _test.exe, e.g.,
```
.\geom_algorithms_test.exe
```