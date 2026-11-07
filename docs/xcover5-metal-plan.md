# XCover5 Metal 계획 (Mali-G52)

작성 2026-11-07. 지금 화면은 CoreAnimation의 OpenGL ES 경로(EAGL 브리지 + Mesa Panfrost)로 그린다. Metal은 그 위에
따로 얹는 것이고, 목표는 Metal이 필요한 것들: 위젯(chronod가 RenderBox로 그림), CoreImage, 그리고 나중에는 화면 합성
자체다. 모든 단계는 부팅 인자로만 켜지고, 꺼져 있으면 지금 GLES 경로가 그대로 동작한다.

## 1. 가져올 곳

| 사례 | GPU | 판정 |
| --- | --- | --- |
| `Luminous418/hakios-a32` (MIT) | **Mali-G52 MC2**, Bifrost, 우리와 같은 GPU | 따라갈 길. y2sios의 런타임과 AIR 컴파일러로 Metal compute, 그리기, CoreAnimation 홈 화면, 위젯(RenderBox `default.metallib`)까지 확인 (`docs/metal-a32.md`) |
| `stormtroopxyz/y2sios` (MIT) | Mali-G77 | 실제 코드: `userland/src/redwoodmetal.m`(런타임, 9800줄), `redwooddrawable.m`, `kernel/tools/mesa/air/`(AIR → NIR → Bifrost 컴파일러 `libredwoodair`) |
| `oosexgang/hakios-xnu-kernel` | Adreno | Metal 드라이버가 Adreno(A6xx) 전용이라 쓰지 않음 |
| `danstronics/ios-s9plus-exynos` | Mali-G72 | metal2vulkan WIP. Mesa 25.0.7의 PanVK는 Bifrost에서 Vulkan 1.0이라 쓰지 않음(그쪽 분석) |
| `LukeValen/panvk-mali-g52` | Mali-G52 | Android/kbase, Mesa 26.x의 PanVK. iOS Metal과 무관, 라이선스 없음 |

y2sios와 A32는 우리와 같은 Mesa 25.0.7 + Gallium panfrost 구성이다.

## 2. 구조

```
Metal 클라이언트(chronod, CoreImage, ...)
  → Metal.framework → IOAcceleratorES의 MetalPluginName/MetalPluginClassName
  → RedwoodMetal.bundle (redwoodmetal.m: MTLDevice, 버퍼, 텍스처, 파이프라인, 인코더)
     ├ GPU 버퍼: IOKit로 커널 GPU 서비스 직접 호출 (6곳: GEM new/info/close, get param, map, surface)
     └ 셰이더와 실행: dlopen libredwoodair.dylib (rwa_* 43개)
          → AIR(애플 셰이더 bitcode) → NIR → panfrost Bifrost 컴파일러 → Gallium panfrost
          → libdrm 대체(우리 herodrm) → XC5GPU → Mali-G52
```

## 3. 단계

**M1. AIR 컴파일러 라이브러리** (mesa-ios CI)
- `gpu/metal/air/` = y2sios `kernel/tools/mesa/air/`(MIT) + A32 `air-integer-ubo.patch`(4 KB 이하 상수 버퍼는 CPU 복사 경로).
- `build-mesa.sh`: `src/compiler/air`로 복사, `subdir('air')`, panfrost 디렉터리에 `libredwoodair` 대상(y2sios
  `air-panfrost.patch`에서 redwood_drm.c 대신 우리 libdrm 대체를 링크). 결과 `lib/libredwoodair.dylib`.
- 성공 기준: CI에서 dylib과 `rwa_*` 43개.

**M2. Metal 런타임 번들**
- `redwoodmetal.m`, `redwooddrawable.m`을 iOS 번들로 빌드. GPU 직접 호출 6곳을 XC5GPU 선택자(0 GetParam, 1 CreateBO,
  2 FreeBO, 3 MapBO, 6 SurfaceBO)로 바꿈. `m2m_log`(148곳)는 XC5GPU Log 선택자로 보내는 작은 대체.
- IOSurface 가져오기: air_metal.c는 IOSurface ID를 prime fd로 넘김(`WINSYS_HANDLE_TYPE_FD`). herodrm의 prime 규약과 맞춤.
- dlopen 경로 `/redwood/lib/...` → `/private/var/xc5/lib/...`.

**M3. 커널과 설치**
- Metal이 플러그인을 찾는 곳: `IOAcceleratorES` 서비스의 `MetalPluginName`, `MetalPluginClassName`(A32 `metal_plugin=1`과
  같은 방식, 부팅 인자 `xc5metal=1`). 번들은 `/System/Library/Extensions/RedwoodMetal.bundle`에 있어야 함(드롭 존 또는 루트).
- 시험 프로그램(A32의 `metal_props`, `metal_drv_test`에 해당)으로 `MTLCreateSystemDefaultDevice()`, compute 한 번 확인.

**M4. 클라이언트별로 켜기**
- 처음에는 시험 프로그램만, 다음에 chronod(위젯), 마지막에 backboardd(화면 합성). A32가 겪은 것: QuartzCore와
  RenderBox의 `default.metallib`(우리는 IPSW 전체라 있을 가능성), Siri 오브의 MPS(SpringBoard에서 nil 처리).

## 4. 위험

1. 런타임이 9800줄이고 y2sios의 kernel 규약(msm/panfrost 요청 번호)에 맞춰져 있음. 6곳만 고치면 되는지는 M2에서 확인.
2. 캐시 일관성: Mali에 IO 일관성이 없음. A32는 사용자 버퍼를 캐시 없이 매핑하고, 제출 때 캐시를 정리함. XC5GPU는
   지금 write-combined 매핑과 SyncBO가 있음.
3. 메모리: RAM 1 GiB. Metal 클라이언트가 늘면 부족할 수 있음.
4. 속도: A32도 CoreAnimation을 Metal로 그릴 때 10~26 fps(CPU 쪽 비용).
