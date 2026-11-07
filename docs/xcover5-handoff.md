# XCover5 인수인계 (에이전트용)

**누가 이어받든(Claude, Codex) 작업을 시작하기 전에 이 파일을 읽고, 작업이 한 단계 끝날 때마다 아래 "지금 상태"와
"다음 행동"을 고친 뒤 커밋할 것.** 부팅별 자세한 기록은 `docs/xcover5-port.md`, GPU는 `docs/xcover5-gpu-plan.md`,
Metal은 `docs/xcover5-metal-plan.md`.

## 사용자와 규칙

- **답은 항상 한국어로.** 사용자는 폰에 플래시하고 콘솔을 영상으로 찍어 보내 줌. 로그는 영상 프레임에서 읽는다
  (`tools/xcover5/dev/frames.ps1`, ffmpeg로 확대).
- 기기 덤프, 시리얼, 애플 바이너리(IPSW, 공유 캐시, metallib)는 git에 넣지 않는다. 공개 저장소: `shin5968/ios-xcover5`
  (remote 이름 `xc5`, 브랜치 `xcover5`). `origin`(Floppikrob/ios-galaxy-s8)에는 push 권한 없음.
- USERDATA 재플래시는 iOS 데이터를 지운다: 사용자가 결정한다. 파괴적인 단계는 먼저 묻는다.
- RAM은 1 GiB로 쓰기로 사용자가 정함(`XC5_DRAM=0x8a2800000:0x40000000`).

## 기기와 구성

Galaxy XCover5 SM-G525N (Exynos 850, Mali-G52 MP1, 720x1480, eMMC). XNU(xnu-8796, ios-galaxy-s8/ios-sweet 계열) +
iPhone X iOS 16.7.16(20H392) 사용자 영역. 8코어, 1 GiB, eMMC PIO(DMA는 `xc5dma=1`로만, 루트 손상 이력 있어 기본 꺼짐).

## 빌드 흐름

1. 커널 변경은 `tools/xcover5/apply-xcover5.py`(+ `XC5GPU.cpp`, `XC5AMFI.cpp`, `xc5_drop.c`)에 한다. 그다음
   `bash tools/xcover5/dev/regen.sh`(WSL)로 `xnu/patches/0125-xcover5.patch`를 다시 만들고 커밋, `git push xc5 xcover5`.
   - 기준 xnu 트리: `../src/xnu-s8`(커밋 91787b5). regen.sh가 reset/clean 후 적용한다.
2. push하면 GitHub Actions: `xnu`(커널, 약 7분, 산출물 `xnu-kernel`), `gpu/**`가 바뀌면 `mesa-ios`(약 3분, 산출물
   `mesa-ios`: Mesa panfrost, herodrm, EAGL 브리지, 주입 라이브러리들, 이제 libredwoodair).
3. 받기: `gh run download ID -R shin5968/ios-xcover5 -n xnu-kernel -D xnu/out`(이전 out은 `xnu/out-oldNN`로 옮김),
   `gh run download ID -n mesa-ios -D shim/xcover5/out/mesa`.
4. 드롭 패키지(CACHE 파티션 → 부팅 때 `/private/var/xc5/`에 설치):
   `python tools/xcover5/mkdrop.py shim/xcover5/out/xc5-drop-mesa.tar --mesa shim/xcover5/out/mesa wallpaper.png=shim/xcover5/out/wallpaper.png`
   (패키지 번호 = 묶은 시각. 같은 번호면 폰이 다시 설치하지 않음.)
5. 이미지: `bash tools/xcover5/dev/mkimage.sh NAME 부팅인자...`(WSL) → `shim/xcover5/out/xc5-ios-NAME-console.tar`.
   사용자는 Odin AP 칸에 tar 하나를 넣는다(boot.img + cache.img).
- PowerShell 5.1 주의: `>`로 저장하면 인코딩/줄끝이 바뀐다. 원본 파일은 WSL `curl`로 받는다. Bash 도구 heredoc은 자주
  깨짐: 스크립트 파일 + `wsl -e bash 파일`.

## 주요 부팅 인자 (커널 `xc5_drop.c`, `apply-xcover5.py`)

| 인자 | 하는 일 |
| --- | --- |
| `xc5gpu=4` | XC5GPU IOKit 서비스(Mali-G52, Mesa용) |
| `xc5gl=2` | backboardd에 `libxc5eagl.dylib`(HakiOS EAGL 브리지) 주입: CoreAnimation을 GLES/Mali로 |
| `xc5wall=1` | SpringBoard에 `libxc5sb.dylib`: PosterKit의 nil `extensionBundleURL` 막기, `SBDisableAutoDim`/`SBDontDimOrLockOnAC`, 고정 그림, 메인 스레드 샘플링 |
| `xc5pbhook=1` | PosterBoard에 `libxc5pb.dylib`(첫 설정 진단·수정) |
| `xc5media=1` | mediaserverd에 `libxc5media.dylib`(인물사진 prewarm 건너뜀) |
| `xc5poster=1` | CollectionsPoster 메인 스레드 샘플링(이게 있으면 SpringBoard가 죽었던 적 있음, 74) |
| `xc5posterretry=1/2` | PosterBoard(와 chronod) 재시작: **SpringBoard abort를 부름, 쓰지 말 것**(73) |
| `xc5ext=0` | AMFI 서비스·0x60·질의 응답 끄기(기본 켜짐) |
| `xc5lsreset=1`, `xc5lsrestore=1` | LaunchServices 저장소 옮기기/되돌리기(70에서 한 번 씀, 지금 폰은 새 저장소 사용 중) |
| `xc5conrows=40` | 화면 아래 40줄만 콘솔 |
| `xc5metal=1` | XC5GPU 아래에 `IOAcceleratorES`("sgx") 공개. Metal 속성 두 개는 비어 있고, 프로세스가 `IORegistryEntrySetCFProperties`로 넣고 뺌(빈 문자열 = 지움). 콘솔 `XC5 metal: ...` |
| `xc5metal=1` | XC5GPU 아래에 `IOAcceleratorES`("sgx") 공개, `MetalPluginName=../../../private/var/xc5/metal/RedwoodMetal`, `MetalPluginClassName=RedwoodMetalDevice`. 두 속성은 `xc5metalfor`에 든 프로세스에게만 보임. 콘솔 `XC5 metal: ...` |
| `xc5metalfor=a,b` | Metal 속성을 볼 프로세스 이름(쉼표, 기본 `chronod`). 다른 프로세스는 Metal 장치가 없다고 봄(78에서 SpringBoard가 잡고 죽음) |
| `xc5mtl=1` | chronod에 `libxc5mtl.dylib`(`gpu/metal/xc5_mtl.c`): 6초 뒤 Metal 장치·버퍼·명령 버퍼 시험, 단계마다 `gl: mtl[pid] step N ...`. 속성을 못 읽을 때만 직접 넣음(옛 커널용). `xc5metal=1` 필요 |
## 지금 상태 (2026-11-07)

- **기준 이미지(부팅 77)**: `xc5-ios-wall2-console.tar` = `xc5gpu=4 xc5gl=2 xc5wall=1`, 커널은 xnu CI 42315969905 산출물,
  드롭 1794022820. CoreAnimation을 Mali로 그리고, 잠금화면에 iOS 기본 배경화면(CollectionsPoster), 잠금 해제 후 홈 화면.
- 안 되는 것: 위젯(chronod/RenderBox, Metal 필요로 추정), 전원 키(드라이버 없음), 셀룰러/카메라 등.
- **Metal 작업 중**(`docs/xcover5-metal-plan.md`): **M1 완료** — mesa-ios CI 42337985007이 `lib/libredwoodair.dylib`
  (9.8 MB, `rwa_*` 43개)을 만듦. y2sios AIR + A32 `air-integer-ubo.patch`, 우리 herodrm과 링크. 첫 시도(42337661505)는
  `redwood_bo_cache_stats`(y2sios panfrost-retain.patch 것) 없음 → `air_metal.c`에 0으로 정의. 폰에서는 아직 안 돌림.
- **M2 완료(커밋 d4c435f, mesa-ios CI 42338725082 성공, 번들 실행 파일 390 KB, 폰에서는 아직 안 돌림)**: y2sios `userland/src/`를 `gpu/metal/runtime/`에,
  `gpu/metal/build-metal.sh`가 `out/metal/RedwoodMetal.bundle/{RedwoodMetal,Info.plist}`를 만듦(mesa-ios 워크플로).
  - 소스는 y2sios 그대로 두고 `runtime/xc5inc/IOKit/IOKitLib.h`(우리 것, `-I`로 SDK보다 먼저)가 바꿈:
    `RedwoodMali`→`XC5GPU`, `RedwoodGPU`→없음(그래서 Mali/panfrost 경로), 구조체 요청 0x44 GET_PARAM / 0x42 CREATE_BO /
    0x09 GEM_CLOSE / 0x7e surface → XC5GPU 선택자 0/1/2/6, `IOConnectMapMemory64`→MapBO(3), Unmap은 아무것도 안 함.
  - `m2m.h`: `/dev/m2m`이 없으면 기록을 XC5GPU Log(5)로 보냄 → 콘솔에 `gl: metal <등급> <분류> ...`.
  - dlopen 경로 `/private/var/xc5/lib/libredwoodair.dylib`. libredwoodair에 rpath `/private/var/xc5/lib` 추가(build-mesa.sh).
  - `air_metal.c`: IOSurface 가져오기를 `herodrm_surface_import`(대리 fd)로. y2sios는 surface ID를 fd 자리에 그대로 넣었음.
  - `_MTLDevice`(Metal 비공개 클래스)는 `-Wl,-U`로 실행 때 찾게 함.
- M3 구성(커밋 feb36fa): `XC5GPU.cpp`의 `IOAcceleratorES`(y2sios 방식), `xc5_drop.c` 주입 표에 chronod
  (`/ChronoCore.framework/Support/chronod`, `xc5mtl`), XPB_MAX 24, `mkdrop.py`가 산출물 `metal/`을 `/private/var/xc5/metal/`에 넣음.
- **M3 폰에서 성공(부팅 78, `xc5-ios-mtl1-console.tar`)**: chronod에서 `RedwoodMetalDevice` 장치, 버퍼(contents 있음, gpuAddress 0),
  명령 큐, 명령 버퍼 commit/wait, `done: Metal device works this far`. `..` 번들 경로가 통함. 자세한 것은 `docs/xcover5-port.md` "Metal".
  - 문제: 속성이 켜진 몇 초 사이 SpringBoard도 장치를 잡고 SIGABRT(SiriUI 파이프라인, MPS 실패, `<MTLDepthStencilState>`). 재시작 후 정상.
  - 조치(커밋 cc29e71, xnu CI 42340653744 + mesa-ios CI 42340653689): 커널이 속성을 처음부터 두고 `xc5metalfor` 프로세스에게만 보여 줌.
- **새 기준(부팅 79)**: `xc5-ios-mtl2-console.tar` = `xc5gpu=4 xc5gl=2 xc5wall=1 xc5metal=1 xc5mtl=1`, xnu CI 42340653744,
  mesa-ios CI 42340653689, 드롭 1794027784. chronod만 Metal을 봄, 시험 step 8 status 4(Completed). **chronod가 위젯을 Metal로
  그려 홈 화면에 배터리 위젯이 처음 나옴**. 사진 위젯은 "대기 중..."(흰 칸). SpringBoard 크래시는 본 프레임에서 없음.

## 다음 행동

1. M4 위젯: 사진 위젯 "대기 중..." 원인 찾기. 위젯 확장(예: PhotosWidget 확장 프로세스)이 스냅샷을 못 주는지, chronod가
   그리다 막히는지. 다음 영상에서 홈 화면 오래 두고 `CRASH`, `metal E ...`, `chronod` 줄 확인. 필요하면 확장 프로세스를
   `xc5metalfor`에 추가(쉼표 목록). `gpuAddress 0`(Mesa 버퍼 경로에서 GPU 주소를 안 알려줌)도 확인할 것.
2. Metal 컴퓨트 한 번(셰이더 metallib을 CI의 Xcode `xcrun metal`로 만들어 드롭에 넣고, `xc5_mtl.c`에서 실행·결과 확인).
3. 안정되면 `xc5mtl=1`(시험)은 빼고 `xc5metal=1`만으로 위젯이 되는지 확인.

## 분석 도구 (`tools/xcover5/dev/`)

- `mntsys.sh`: IPSW의 SystemOS/OS를 apfs-fuse로 마운트(WSL `~/xc5/ipsw`).
- `dsc.py SLIDE 주소...`: 콘솔 `CRASH … slide … bt …` 주소를 공유 캐시 함수로. `dsdis.sh 주소 길이`: 디스어셈블.
  `dsstr.py 주소`: 문자열/CFString. 크래시 메시지는 커널이 `STR …` 줄로 힙 문자열을 찍어 줌(예외 이유가 거기 나옴).
- `frames.ps1`: 영상 → 프레임 묶음.
