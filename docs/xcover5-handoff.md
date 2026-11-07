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

## 지금 상태 (2026-11-07)

- **기준 이미지(부팅 77)**: `xc5-ios-wall2-console.tar` = `xc5gpu=4 xc5gl=2 xc5wall=1`, 커널은 xnu CI 42315969905 산출물,
  드롭 1794022820. CoreAnimation을 Mali로 그리고, 잠금화면에 iOS 기본 배경화면(CollectionsPoster), 잠금 해제 후 홈 화면.
- 안 되는 것: 위젯(chronod/RenderBox, Metal 필요로 추정), 전원 키(드라이버 없음), 셀룰러/카메라 등.
- **Metal 작업 중**(`docs/xcover5-metal-plan.md`): M1 = `libredwoodair.dylib`(y2sios AIR + A32 패치)를 mesa-ios에서 빌드.
  CI 42337661505에서 컴파일은 되고 링크에서 `redwood_bo_cache_stats` 하나가 없었음 → `air_metal.c`에 정의 추가,
  CI 42337985007 결과 확인 필요.

## 다음 행동

1. mesa-ios 산출물에 `lib/libredwoodair.dylib`와 `rwa_*` 함수 수(빌드 로그 `libredwoodair: N rwa_* functions`) 확인.
2. M2: y2sios `userland/src/redwoodmetal.m`, `redwooddrawable.m`(MIT)을 `gpu/metal/runtime/`으로 가져와 iOS 번들
   `RedwoodMetal.bundle`로 빌드. GPU 직접 호출 6곳(kMsmGemNew/Info, kDrmGemClose, kMsmGetParam, IOConnectMapMemory64,
   surface)을 XC5GPU 선택자(0 GetParam, 1 CreateBO, 2 FreeBO, 3 MapBO, 6 SurfaceBO)로, `m2m_log`는 XC5GPU Log(5)로,
   dlopen 경로 `/redwood/lib` → `/private/var/xc5/lib`.
3. M3: 커널에 `xc5metal=1` → `IOAcceleratorES`에 `MetalPluginName=RedwoodMetal`, `MetalPluginClassName=RedwoodMetalDevice`.
   번들을 `/System/Library/Extensions/`에 두는 방법(드롭 존은 `/private/var/xc5`만 씀).

## 분석 도구 (`tools/xcover5/dev/`)

- `mntsys.sh`: IPSW의 SystemOS/OS를 apfs-fuse로 마운트(WSL `~/xc5/ipsw`).
- `dsc.py SLIDE 주소...`: 콘솔 `CRASH … slide … bt …` 주소를 공유 캐시 함수로. `dsdis.sh 주소 길이`: 디스어셈블.
  `dsstr.py 주소`: 문자열/CFString. 크래시 메시지는 커널이 `STR …` 줄로 힙 문자열을 찍어 줌(예외 이유가 거기 나옴).
- `frames.ps1`: 영상 → 프레임 묶음.
