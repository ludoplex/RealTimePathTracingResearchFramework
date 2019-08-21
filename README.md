# rtobj

An example of OBJ rendering with OSPRay, Embree and OptiX.
Uses [tinyobjloader](https://github.com/syoyo/tinyobjloader) to load OBJ files.

## Ray Tracing Backends  

The currently implemented backends are: OSPRay, Embree and OptiX.
When running the program, you can pick which backend you want from
those you compiled with by specifying it as the first argument on
the command line:

```
./rtobj <backend> <mesh.obj>
```

All three ray tracing backends use [SDL2](https://www.libsdl.org/index.php) for window management
and [GLM](https://glm.g-truc.net/0.9.9/index.html) for math.
If CMake doesn't find your SDL2 install you can point it to the root
of your SDL2 directory by passing `-DSDL2=<path>`.
Similarly for GLM, you can point it to the glmConfig.cmake file
in your GLM distribution by passing `-Dglm_DIR=<path>`.
To track and report statistics about the number of rays traced per-second
run CMake with `-DREPORT_RAY_STATS=ON`. Tracking these statistics can
impact performance slightly.


rtobj only supports per-OBJ group/mesh materials, OBJ files using per-face materials
can be reexported from Blender with the "Material Groups" option enabled.

### Embree

Dependencies: [Embree](https://embree.github.io/),
[TBB](https://www.threadingbuildingblocks.org/) and [ISPC](https://ispc.github.io/).

To build the Embree backend run CMake with:

```
cmake .. -DENABLE_EMBREE=ON \
	-Dembree_DIR=<path to embree-config.cmake> \
	-DTBB_DIR=<path TBBConfig.cmake> \
	-DISPC_DIR=<path to ispc>
```

You can then pass `-embree` to use the Embree backend. The `TBBConfig.cmake` will
be under `<tbb root>/cmake`, while `embree-config.cmake` is in the root of the
Embree directory.

### OptiX

Dependencies: [OptiX 7](https://developer.nvidia.com/optix), [CUDA 10](https://developer.nvidia.com/cuda-zone).

To build the OptiX backend run CMake with:

```
cmake .. -DENABLE_OPTIX=ON
```

You can then pass `-optix` to use the OptiX backend.

If CMake doesn't find your install of OptiX you can tell it where
it's installed with `-DOptiX_INSTALL_DIR`.

### DirectX Ray Tracing

If you're on Windows 10 1809, have the 10.0.17763 SDK and a DXR capable GPU you can also run
the DirectX Ray Tracing backend.

To build the DXR backend run CMake with:

```
cmake .. -DENABLE_DXR=ON
```

You can then pass `-dxr` to use the DXR backend.

### OSPRay

Dependencies: [OSPRay](http://www.ospray.org/).

To build the OSPRay backend run CMake with:

```
cmake .. -DENABLE_OSPRAY=ON -Dospray_DIR=<path to osprayConfig.cmake>
```

You can then pass `-ospray` to use the OSPRay backend.

## Citation

If you find rtobj useful in your work, please cite it as:

```
@misc{rtobj,
	author = {Will Usher},
	year = {2019},
	note = {https://github.com/Twinklebear/rtobj},
	title = {rtobj}
} 
```

## Images

These versions of Sponza and San Miguel are from Morgan McGuire's [Computer Graphics Data Archive](https://casual-effects.com/data/).

![San Miguel](https://i.imgur.com/rhzcwaC.png)

![Sponza](https://i.imgur.com/RxNP15S.png)

