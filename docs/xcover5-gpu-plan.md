# XCover5 GPU 가속 계획 (Mali-G52)

작성 2026-11-04. 아직 GPU 코드는 없음. 이 문서는 참고 구현 분석과 작업 순서만 정리한다.

## 1. 결론

iOS 16의 CoreAnimation 렌더 서버(backboardd)는 Metal 장치를 만들지 못하면 **스스로 OpenGL ES 렌더러
(`CA::OGL::GLESContext`)로 돌아간다**. 그래서 Metal 드라이버를 만들 필요가 없다. OpenGL ES만 Mali에서
돌리면 된다. Galaxy S7 포트(`wardevsa/ios-a5xelte`, 브랜치 `herolte`, Mali-T880)가 이 경로로 iOS UI를
GPU로 그리는 데 성공했다. XCover5는 이 구성을 그대로 따르고, 바뀌는 부분은 **GPU 세대(Midgard → Bifrost)**와
**Exynos 850의 전원/주소**다.

```
SpringBoard 등 ─ CoreAnimation(GLES 경로) ─ EAGL 브리지(dylib, backboardd에 주입)
   ─ Mesa Panfrost(OSMesa, iOS용 빌드) ─ libdrm 대체(IOKit 호출) ─ 커널 GPU 서비스 ─ Mali-G52
```

## 2. 참고 구현(herolte) 분석

| 부분 | 파일(herolte 브랜치) | 하는 일 | 라이선스 |
| --- | --- | --- | --- |
| 커널 GPU 코어 | `bsd/dev/herolte_gpu.c` (패치 0231, 0235, 0239, 0243, 0247-0267, 0307) | 전원/클럭 확인, 코어 전원 켜기, AS0 페이지 테이블, 작업 슬롯 제출(폴링), 결함 복구, 프레임 GPU 축소 | APSL (XNU 파생) |
| 커널 IOKit 면 | `iokit/Kernel/arm/HeroGPU.cpp` (0239, 0248) | `IOServiceMatching("HeroGPU")`, 선택자 0 GetParam, 1 CreateBO, 2 FreeBO, 3 MapBO, 4 Submit, 5 Log, 6 SurfaceBO(IOSurface 가져오기), 7 SyncBO(캐시 정리) | APSL |
| libdrm 대체 | `gpu/mesa/herodrm.c` | Mesa가 부르는 Panfrost DRM ioctl을 HeroGPU 호출로 바꿈. 제출은 동기식이라 syncobj는 항상 완료 | MIT |
| Mesa 빌드 | `gpu/mesa/build-mesa.sh`, `patch-mesa.py`, `.github/workflows/mesa-ios.yml` | Mesa 25.0.7을 macOS 러너에서 iOS arm64로 교차 빌드. Gallium panfrost + OSMesa, LLVM 없음. mmap을 herodrm으로 우회하는 등 작은 패치 | MIT |
| EAGL 브리지 | `gpu/eagl/`(Bergamota), `gpu/hakios-gl/`(HakiOS) | `EAGLContext`를 OSMesa 위에 구현. iOS 16 OpenGLES.framework의 457개 진입점 오프셋 확인됨 | MIT |
| 주입 | `bsd/dev/herolte_gl.c` (0245) | launchd가 backboardd를 exec할 때 커널이 `DYLD_INSERT_LIBRARIES=.../libheroeagl.dylib`를 붙임. 부팅당 처음 2번만, 실패하면 3번째부터 순정 | APSL |
| 파일 전달 | `bsd/dev/herolte_drop.c` | USERDATA의 "drop zone"에서 dylib을 `/private/var/herolte/lib`로 꺼냄. 루트 이미지를 다시 굽지 않음 | APSL |

측정 결과(그쪽 문서): HeroGPU WRITE_VALUE 115 µs, Mesa로 256×256을 159 fps, iPhone 8 해상도(750×1334) +
GPU 축소로 "훨씬 부드러움", CPU 유휴 7-22 %. 해결해야 했던 문제: ASTC 텍스처(GPU 지원 비트를 숨겨 Mesa가 CPU로
해제), P3 색 공간(ADT `artwork-display-gamut = "sRGB"`), 다른 프로세스가 CPU로 그린 IOSurface의 캐시 정리
(Mali가 IO 일관성이 없음), 부트로더가 DECON 화소 클럭을 26 MHz로 남겨 실제 20 Hz였던 것.

## 3. XCover5 하드웨어 (순정 DT `research/bootdtb_0.dts`)

- GPU: `mali@11500000`, 크기 0x5000, `compatible = "arm,mali"`. **Mali-G52**(Bifrost, Job Manager 방식).
- 인터럽트: JOB = SPI 0x7e(126), MMU = 0x7f(127), GPU = 0x7d(125).
- 전원 도메인: `pd-g3d@11861d00`, `need_smc = <0x11410204>` → 켜고 끄기에 **보안 모니터 호출이 필요할 수 있음**.
  상태: `gpu_pmu_status_reg_offset = 0x1d04`, 마스크 1 → PMU(0x11860000) + 0x1d04 = **0x11861d04 비트 0**.
- 클럭: CMU G3D, `g3d_cmu_cal_id = 0xb040004`(CAL/ACPM 경유 DVFS). DVFS 표: 1001 / 865 / 754 / 598 / 433 / 377 MHz.
- 온도 센서: TMU `G3D@10070000`.

## 4. T880(Midgard) → G52(Bifrost)에서 바뀌는 것

| 항목 | herolte(T880) | XCover5(G52)에서 할 일 |
| --- | --- | --- |
| MMU 페이지 테이블 | Midgard 형식(L3 PTE `| 0x2c1`, 테이블 `| 3`) | **AArch64 4K 형식**: `AS_TRANSCFG`(AS + 0x30)에 AArch64 모드, PTE 속성(AF, SH, AttrIndx, AP)과 MAIR 값. Linux panfrost_mmu.c의 Bifrost 경로를 따름 |
| GetParam | `THREAD_TLS_ALLOC`, `AFBC_FEATURES`를 0으로 | Bifrost(v7)는 실제 레지스터(TLS 0x310, AFBC 0x4c)를 돌려줘야 Mesa가 바르게 설정 |
| 작업 제출 | JS0/JS1, 폴링 | 같음(G52는 Job Manager). 작업 기술자는 Mesa가 만듦 |
| 셰이더 컴파일러 | Midgard | Mesa의 **Bifrost 컴파일러**(같은 빌드에 포함, 설정만 확인) |
| 전원/클럭 | S-Boot가 켜 둠, 커널이 BUCK/클럭 조정 | 먼저 S-Boot가 켜 두는지 확인. 꺼져 있으면 SMC/ACPM 경로 조사 필요(가장 큰 미지수) |
| 캐시 일관성 | 없음(`COHERENCY_FEATURES=0`) | 확인 필요. 없으면 같은 방식(쓰기 결합 매핑 + 명시적 정리) |
| 화면 출력 | 1440×2560 command 모드 DECON | 720×1480 video 모드. 우리 `SweetFramebuffer` present 경로에 GPU 축소를 붙임 |

## 5. 단계

각 단계는 부팅 인자로만 켜지고(기본 꺼짐), 꺼져 있으면 지금의 CPU 경로가 그대로 동작한다.

**0단계: 읽기만 하는 확인** (`xc5gpu=1`)
- PMU 0x11861d04 비트 0을 읽음. 켜져 있을 때만 0x11500000을 매핑해 GPU_ID, SHADER/TILER/L2_PRESENT,
  MMU_FEATURES, COHERENCY_FEATURES, AS/JS_PRESENT를 하트비트 로그에 출력. 쓰기 없음.
- 성공 기준: GPU_ID의 제품 번호가 G52(0x7402)로 보임. 꺼져 있으면 1단계에서 전원 조사.

**1단계: 코어 전원과 WRITE_VALUE** (`xc5gpu=2`)
- L2 → 셰이더 → 타일러 순서로 PWRON, 준비 비트 폴링(유한). AS0에 AArch64 페이지 테이블, 개인 버퍼 하나,
  WRITE_VALUE 작업 하나를 JS1로 제출. JOB/MMU/GPU 결함 레지스터 로그.
- 성공 기준: 센티널 값이 바뀌고 결함 0.

**2단계: HeroGPU 서비스 이식** (`xc5gpu=3`)
- `HeroGPU.cpp`(선택자 0-7)를 `XC5GPU`로 이식. GetParam을 Bifrost 레지스터로, BO 생성/매핑/해제, Submit,
  SurfaceBO(우리 `SweetSurface` 사용), SyncBO. 사용자 VA 범위와 L3 테이블 풀은 GPU 전용 블록에(herolte 0307의 교훈).
- 성공 기준: 커널 자체 테스트(사용자 버퍼로 WRITE_VALUE) 통과.

**3단계: Mesa(Bifrost)와 herodrm 빌드**
- `mesa-ios` 워크플로를 가져와 Mesa 25.0.x panfrost를 iOS arm64로 빌드. herodrm의 매개변수/선택자를 XC5GPU에 맞춤.
- 파일 전달: **drop zone** 필요. CACHE 파티션(200 MiB)의 로그 영역 뒤를 쓰는 방식을 우선 검토(루트 이미지 재플래시 없이 dylib 교체).
- 성공 기준: `osmesa-test`가 삼각형을 그리고 fps를 로그에 남김.

**4단계: CoreAnimation을 GPU로** (`xc5gl=1`)
- EAGL 브리지(gl_hero.c 포함)를 빌드, 커널이 backboardd에 주입(처음 2번만, 실패 시 순정 복귀).
- ADT에 `artwork-display-gamut = "sRGB"`, MobileGestalt 캐시 처리, ASTC 숨김, 공유 IOSurface 캐시 정리를 그대로 적용.
- 성공 기준: 잠금화면/홈 화면이 GPU로 그려지고 `swap` 빈도와 CPU 사용률이 개선.

**5단계: 해상도와 출력 마무리**
- iOS를 iPhone 8 크기(750×1334)로 그리게 하고 GPU로 패널 크기에 맞춤(픽셀 2.7배 감소).
- DECON 화소 클럭이 낮게 남아 있는지 확인(herolte는 26 MHz로 20 Hz였음).
- CPU 클럭(DVFS)은 별도 작업.

## 6. 위험과 미지수

1. **G3D 전원**: S-Boot가 꺼 둔다면 `need_smc`/ACPM 경로를 알아내야 한다. 잘못된 쓰기는 외부 abort로 정지할 수 있어서
   0단계는 상태 비트만 읽는다.
2. **Bifrost MMU 형식**: 속성 비트가 틀리면 MMU 결함. 1단계 로그로 바로 보인다.
3. **IO 일관성 없음**: 캐시 정리를 빠뜨리면 검은 화면이나 깨진 텍스처(herolte가 겪은 문제).
4. **빌드 환경**: Mesa는 iOS SDK가 있는 macOS가 필요하다. GitHub Actions macOS 러너로 해결(커널과 같은 방식).
5. **메모리**: Mesa는 타일러 힙으로 큰 버퍼를 요구한다(herolte는 32 MiB로 제한). 지금 RAM은 1 GiB.
6. **간헐적 잠금 해제 실패**는 GPU와 별개로 남아 있다.

## 7. 순서와 대략의 규모

0-1단계: 커널만, 며칠. 2단계: 1주 안팎. 3단계: 빌드 파이프라인과 drop zone 포함 1-2주. 4단계: 1-2주(대부분 재사용,
iOS 쪽 문제 해결이 시간을 씀). 5단계: 며칠. 각 단계마다 폰 시험이 필요하다.

## 8. 바로 다음 행동

0단계 커널(`xc5gpu=1`, 읽기만)을 만들어 GPU 전원 상태와 GPU_ID를 확인한다.
