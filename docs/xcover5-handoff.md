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
| `xc5pmic=1/2` | S2MPU12 PMIC를 ACPM으로(`xc5_pmic.c`): 1 = 레지스터 덤프 + 바뀌는 레지스터 출력(전원 키 찾기), 2 = `xc5pwrreg`/`xc5pwrbit`(`xc5pwrlow`)를 30 ms마다 읽어 전원 키로 |
| `xc5ckpt=1` | checkpointd에 `libxc5ckpt.dylib`(FairPlay mach_msg 3초 제한) |
| `xc5photos=1` | 사진 앱·checkpointd·assetsd에 샘플러(`libxc5poster.dylib`) |
| `xc5metalfor=a,b` | Metal 속성을 볼 프로세스 이름(쉼표, 기본 `chronod`). 다른 프로세스는 Metal 장치가 없다고 봄(78에서 SpringBoard가 잡고 죽음) |
| `xc5mtl=1` | chronod에 `libxc5mtl.dylib`(`gpu/metal/xc5_mtl.c`): 6초 뒤 Metal 장치·버퍼·명령 버퍼 시험, 단계마다 `gl: mtl[pid] step N ...`. 속성을 못 읽을 때만 직접 넣음(옛 커널용). `xc5metal=1` 필요 |
## 지금 상태 (2026-11-07)

- **RTC2 진단 이미지 완료**: 커밋 d90f1584, xnu CI 42355742720 성공. `shim/xcover5/out/xc5-ios-rtc2-console.tar` (74,506,240 bytes), SHA256 `c42d042a9bd844c933ac623d4fd593b96c8f9387f33f0bd9ac2d9566a2595cf4`. boot.img + 기존 cache.img만 포함(USERDATA 없음). 명시적 `xc5rtc=1`, 기존 wall2/metal/ckpt/pmic 설정 유지. `XC5 RTC2`가 진입/ACPM/원시 레지스터/설정 전·요청·readback 시간을 출력. PMIC 쓰기와 날짜 변환 변경 없음. 호스트 정상·읽기 실패·잘못된 날짜 및 별도 복사본 롤백 시험 통과; 실기기 RTC 교정 성공은 아직 미확인. 165706 영상의 launchd 날짜는 1969-12-31이며 이전 RTC 시작 로그는 확인되지 않음.

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
  mesa-ios CI 42340653689, 드롭 1794027784. chronod만 Metal을 봄, 시험 step 8 status 4(Completed). **chronod가 Metal로 무언가를
  그림**(`metal W PERF slow draw ... plane_color`). 배터리 위젯은 전부터 나오던 것(Metal과 무관, 사용자 확인). 사진 위젯
  "대기 중..."은 사진 앱도 제대로 안 되므로 사진 라이브러리(assetsd/photolibraryd) 쪽 문제일 가능성. SpringBoard 크래시는 본 프레임에서 없음.

- **사진 앱 조사는 사용자 요청으로 중단(2026-11-07)**: 부팅 84에서 checkpointd/FairPlay는 넘었고 남은 건 assetsd(빈 보관함).
  photo8 이미지(assetsd 샘플링)는 만들어 두었으나 시험 안 함.
- **드라이버 작업(사용자 요청)**: 남은 것 = 전원 키(PMIC), RTC(PMIC, 1970년), 배터리 잔량(s2mu106 연료계), 밝기, 진동,
  Wi-Fi/소리/카메라/셀룰러(큼). 전원 키부터: `tools/xcover5/xc5_pmic.c`(커밋 98f6ca3, xnu CI 42350979144) =
  S2MPU12를 ACPM으로 읽기(메일박스 0x11900000, SRAM 0x02039000, initdata 0x7000, 채널 2; 프로토콜은 y2sios/Scion 문서대로,
  코드 복사 없음, **읽기만**). 시험 이미지 `xc5-ios-pmic1-console.tar`(wall2 기준 + `xc5metal=1 xc5ckpt=1 xc5pmic=1`).

- **전원 키는 이미 XCover 키가 대신함**(사용자: 그걸로 충분) → PMIC 전원 키(`xc5pmic=2`)는 보류.
- **날짜(RTC)**: `xc5_pmic.c`가 부팅 때(launchd 전) PMIC RTC 블록 2의 0x00-0x0B를 ACPM으로 읽고(Scion이 정리한 S2MPS18
  배치: CTRL, UPDATE, CAPSEL, MSEC, SEC, MIN, HOUR, WEEK, DAY, MONTH, YEAR(2000+), 이진) S-Boot가 잡아 둔 값에 업타임을
  더해 `clock_set_calendar_microtime`. 2024-2099가 아니면 안 씀. 기본 켜짐(`xc5rtc=0`으로 끔). 커밋 b6f3524, xnu CI
  42351773001. 시험 이미지 `xc5-ios-rtc1-console.tar`(+ `xc5pmic=1`이라 PMIC/RTC 레지스터 덤프도 찍힘). 볼 것:
  `XC5 RTC: CTRL .. YEAR ..`, `XC5 RTC: the PMIC's clock says ...: calendar set`, 잠금화면 날짜.
  - 사례 조사(2026-11-07): **Samsung의 S2MPU12 RTC 드라이버 원본**이 Galaxy A21s 커널(같은 Exynos 850,
    GitHub `jameskdev/android_kernel_samsung_a21s-common` `drivers/rtc/rtc-s2mpu12.c`, `include/linux/mfd/samsung/rtc-s2mp.h`,
    `drivers/mfd/s2mpu12-core.c`; GPL, 사실만)에 있음: RTC = I2C 주소 2, `CONFIG_RTC_DRV_S2MP` 없음 → MSEC 0x04, SEC 0x05 ..
    YEAR 0x0B, 시 `& 0x1f`(PM 비트 6), 연도 +2000, CTRL 비트0 BCD/비트1 24h. **읽기 전마다 UPDATE(0x02) |= RUDR(비트 0) 쓰고
    1 ms 뒤 읽음**(안 하면 마지막 래치 값). 안드로이드는 `CONFIG_RTC_HCTOSYS=y`로 부팅 때 이 시계로 시간을 맞춤.
    Scion(S9, S2MPS18)은 같은 배치, RUDR 안 쓰고 S-Boot 래치 + 업타임(폰에서 2018-01-01이 나왔다고 적음, 즉 시계 미설정).
    y2sios/Scion은 RTC 없을 때 `epoch=` 부팅 인자/`/chosen/boot-epoch`로 PE의 시간 함수를 채움(`GenericARMPE.cpp`).
  - 조치(커밋 12d416e, xnu CI 42357159161): 우리도 RUDR 래치를 한 번 씀(`xc5rtclatch=0`이면 읽기만), 시 마스크 0x1f.
    시험 이미지 `xc5-ios-rtc2-console.tar`(rtc1 대체). 볼 것: `XC5 RTC: latched (UPDATE 0x.. | RUDR): 0` 다음 날짜.
  - (Codex가 사이에 d90f158 `XC5 RTC2` 진단 줄 추가, 69fe4d4 문서. 내 12d416e는 그 위에 있음.)
  - **rtc2 결과(영상 20261107_180915)**: launchd 시각 여전히 `1969-12-31 16:00:31`(PST) → 달력 안 맞음. 부팅 초기 RTC 줄은
    화면이 heartbeat 블록이라 안 보임. `XC5 PMIC: pmic 01: 00 -> 10 / 10 -> 00`(xc5pmic=1 감시)이 계속 나와 **ACPM 읽기는 동작**.
  - 조치(커밋 53fb077, xnu CI 42358950388): rtc_sync 결과를 저장하고 스레드가 **20초마다 3분간 다시 찍음**(거부 시 원시 값 포함,
    설정 시 readback, 현재 달력 값). 달력이 아직 2020년 전이면 같은 값 + 업타임으로 **다시 설정**. 이미지 `xc5-ios-rtc3-console.tar`
    (xc5pmic 감시는 뺌). 볼 것: 부팅 후 `XC5 RTC: ...; calendar now ...` 줄과 잠금화면 날짜.
  - **rtc3 결과(영상 182822)**: `XC5 RTC: 2022-01-01 12:01:42 is not a date to trust (CTRL 02 raw 2a 01 4c 01 01 16);
    calendar now 70 (uptime 70 s)` → 읽기는 정상, **PMIC 시계가 DT의 init_time(2022-01-01 12:00)에서 돌고 있음**(안드로이드가
    더는 네트워크 시간으로 맞춰 주지 않음; Scion S9의 2018-01-01과 같은 상황).
  - 조치(커밋 5b9aba2, xnu CI 42360462965): `mkimage.sh`가 `xc5epoch=<PC 시각>`을 자동으로 넣음. RTC가 무효이거나 xc5epoch보다
    뒤면 달력 = xc5epoch + 업타임, 그리고 그 시각을 **RTC에 씀**(SEC..YEAR + UPDATE |= WUDR(비트 1), 삼성 드라이버 방식;
    `xc5rtcwrite=0`이면 안 씀). 다음 부팅부터는 RTC가 맞는 시각을 갖고 있어 새 이미지 없이도 유지(PMIC가 재부팅 때 시계를
    지우지 않는다면). 오차: 이미지를 만든 뒤 부팅까지 걸린 시간만큼 늦음. 이미지 `xc5-ios-rtc4-console.tar`.
    볼 것: `XC5 RTC: written 2026-..`, `... not valid; xc5epoch ...: using ...; RTC written`, 잠금화면 날짜. 그리고 **한 번 더
    재부팅**했을 때 `the PMIC's clock said 2026-..`가 나오는지(시계가 유지되는지).
  - **rtc4 결과(영상 20261107_184810): 날짜 해결.** 잠금화면 `11월 7일 토요일 1:46`, launchd 로그 `2026-11-07 01:45`.
    시각은 iOS 기본 시간대(미국 태평양, PST) 기준이라 한국 시각과 8시간 차이(UTC 09:46 = KST 18:46, 맞음). 시간대는
    설정 > 일반 > 날짜 및 시간에서 바꾸거나, 이후 `/var/db/timezone/localtime` 쪽으로 기본값을 줄 수 있음.
    재부팅 뒤 RTC 유지 여부(새 이미지 없이 `the PMIC's clock said 2026-..`)는 아직 확인 안 함.
- **Wi-Fi 조사**: 칩은 Exynos 850 내장 Samsung SCSC "WLBT"(`scsc_wifibt@119C0000`, 예약 메모리 `wifibt_if` 12 MB,
  인터럽트 MBOX/ALIVE/CFG_REQ/WDOG, syscon: wlbt_remap 0x14C50000, boot_cfg 0x14C60000, BAAW들). Broadcom이 아니라서
  S9(brcmfmac)·y2sios 하드웨어 드라이버는 못 씀. iOS 쪽 층(en0 + 가짜 IO80211Controller + apple80211 ioctl, y2sios
  `RedwoodWLAN.cpp`/`redwood_apple80211.c`, Scion `drivers/wifi/hakios.apple80211.c`)은 재사용 가능. 하드웨어 쪽 참고 소스:
  GitHub `jameskdev/android_kernel_samsung_a21s-common`(Exynos 850 = S5E3830, GPL, 사실만 읽기)
  `drivers/misc/samsung/scsc/platform_mif_3830.c`, `mif_reg_S5E3830.h`, `mxman.c`(SCSC 코어 약 1.4 MB), `drivers/net/wireless/scsc`
  (약 3 MB). 펌웨어는 순정 AP tar의 vendor에서: `tools/xcover5/dev/stockfw.sh`(+ `lpunpack.py`) → WSL `~/xc5/stock/vendor`
  (git에 넣지 않음).
  - 추출 결과(2026-11-07): `vendor/wifi/mx140.bin`(1,573,842 B, 헤더 "smxf", WLBT 펌웨어), `mx140_t.bin`(다른 변형),
    `mx140_wlan.hcf`/`mx140_bt.hcf` 등 설정, `hydra_config.sdb`, `slsi_reg_database.bin`, `platform.txt` =
    `nacho_s612_xcover5_kor`, build_id `S612_E3830_R4-1_Y22W51A`(코어 이름 S612). `vendor/firmware`에 터치·오디오(ABOX,
    aw8896 앰프)·카메라 펌웨어도 있음.
  - 규모: SCSC 코어 + 무선 드라이버 합쳐 원본 약 4.5 MB(GPL, 사실만 참고). 단계: (1) 펌웨어를 예약 메모리에 넣고 WLBT를
    리셋 해제해 메일박스 응답 받기, (2) MX 관리 채널, (3) HIP(데이터 경로)와 MLME(검색·접속), (4) iOS 쪽 en0/apple80211.
- **Wi-Fi 1단계(커밋 f262938, xnu CI 42362014245)**: `tools/xcover5/xc5_wlbt.c`, 부팅 인자 `xc5wifi=1`. WLBT 코어를 켜고
  (PMU 0x11860000: CTRL_S.START, OPTION.DATA, TOP_OUT.PWRRGTON_CP, VGPIO_TX_MONITOR(0x11A50000+0x1700)[29] 대기,
  CONFIGURATION.LOCAL_PWR_CFG, STATUS[0] 대기, CTRL_NS), CFG_REQ(SPI 0xE6, GIC 보류 비트로 감시) 뒤 SMC 0x82000710(1<<32),
  WLBT_REMAP+0x400, DBUS BAAW(0x14C20000) 창 0 = WLBT 0x80000000-0x80C00000 → 공유 메모리, PBUS BAAW(0x14C30000) 창 0/1
  (메일박스), BOOT_CFG(0x14C60000) KA 패치 + CFG_ACK. 펌웨어는 공유 메모리(**물리 0x880000000, 12 MiB**: 로더·XNU가 안 쓰는
  상위 뱅크) 맨 앞, AP2WLBT 메일박스(0x119C0000) ISSR0 = 진입점(헤더 +40, mx140은 0x1a9), ISSR1 = 0(mxconf 아직 없음),
  ISSR2 = 0xbcdeedcb, ISSR3 = 0. 그 뒤 20초 동안 PMU WLBT_STATES/STATUS, 메일박스 INTSR/ISSR, SPI(MBOX/ALIVE/CFG/WDOG) 보류,
  펌웨어 panic record(헤더 +176 오프셋) 출력. 출처: 삼성 SCSC 드라이버(A21s 커널 platform_mif_3830.c 등, GPL, 사실만).
  펌웨어와 KA 패치(8051 코드 57워드)는 저장소에 넣지 않고 `tools/xcover5/dev/wlbtfw.sh`가 로컬에서 만듦
  (`shim/xcover5/out/wifi/`), 드롭 `/private/var/xc5/wifi/mx140.bin`, `ka.bin`. 이미지 `xc5-ios-wifi1-console.tar`
  (드롭 1794046067). mxconf(관리 채널 설정)가 없으니 펌웨어는 곧 panic할 것으로 예상: 1단계 목표는 "코어가 펌웨어를
  실행한다"는 증거(STATES 변화, 메일박스 인터럽트, panic record).

## 다음 행동

- **wifi1 결과(영상 191101)**: 커널 panic `kmem_alloc(..., 12582912, 0x100021) failed with 3 @vm_kern.c:178`
  (12 MiB 한 번에 `ml_io_map_wcomb` → 커널 맵 자리 없음, 실패 시 panic). WLBT 단계는 시작도 못 함. 같은 영상에서
  **RTC는 재부팅 뒤에도 유지**: `the PMIC's clock said 2026-11-07 10:08:25 UTC`(rtc4가 써 둔 시각).
  조치(커밋 9699df6, xnu CI 42363089520): 공유 메모리 앞 4 MiB만 매핑(`SHMEM_MAP`). 이미지 `xc5-ios-wifi2-console.tar`
  (드롭은 wifi1과 같음 1794046067, 이미 설치됐으면 그대로).
- **wifi2 결과(영상 20261107_192345)**: 패닉 없이 부팅, 잠금화면까지. WLBT 보고(t+5000, t+15000 ms 같음):
  `STATUS 0x1 STATES 0x10 CONF 0x1 OPT 0 CTRL_NS 0x21 CTRL_S 0x8 DEBUG 0 | mbox INTSR0 0 INTMSR0 0 INTSR1 0 INTMSR1 0
  ISSR0-3 0x22220004 0 0xbcdeedcb 0 | spi mbox 0 alive 0 cfg 0 wdog 0`. **ISSR0이 우리가 쓴 진입점 0x1a9에서 0x22220004로
  바뀜** → WLBT 쪽(부트 ROM 또는 펌웨어)이 실행돼 메일박스에 씀(살아 있다는 증거). panic record(@0x1930a4)는 0, 메일박스
  인터럽트 없음. 0x22220004의 뜻은 삼성 소스에 없음(상태/오류 코드로 추정). 앞쪽 줄(firmware/power/CFG_REQ/SMC)은 콘솔이
  밀려서 영상에 없음. 추정: mxconf가 없어서(ISSR1=0) 거기서 멈춤.
  RTC: `the PMIC's clock said 2026-11-07 10:21:09 ..., behind xc5epoch 1794046920: using 1794046950; RTC written`
  (rtc4가 쓴 값이 이미지 시각보다 약간 뒤 → 이미지 시각 사용, 정상 동작).
- **다음 = Wi-Fi 2단계**: mxconf(mxconf.h: magic, version, mx_trans_conf = mxmgmt 전송의 두 방향 스트림, mxlog, gdb 전송,
  fwconfig)를 공유 메모리(4 MiB 매핑 안, 펌웨어 런타임 0x233df0 뒤)에 만들고 ISSR1 = 그 오프셋. mxmgmt 전송은
  mifstream(cpacket_buffer 링 + 메일박스 인덱스 + 인터럽트 비트, mxmgmt_transport.c, mifstream.c, cpacket_buffer.c,
  mifintrbit.c, mifmboxman.c). 목표: 펌웨어가 `MM_START_IND`(mxman.c 528/852) 보냄. 필요하면 매핑을 2 MiB 단위로 늘림.
- **Wi-Fi 2단계 구현(커밋 ea9ee60, xnu CI 42364390385)**: `xc5_wlbt.c`가 mxconf(240 B, magic 0x79828486, 0.5)를 펌웨어 런타임 뒤
  (4 KiB 정렬)에 만들고 ISSR1 = 그 오프셋. 스트림(transports_init 순서와 같은 인터럽트 비트): mgmt to-AP 512 B(128 B×4,
  read bit 1 / write bit 0), from-AP(1/2), gdb R4(2 KiB, 3/2 · 3/0), gdb M4(1/4 · 5/0), mxlog 16 KiB(4/6), gdb M4_1 미사용(0).
  to-AP 버퍼는 0xff로 채움. INTMR0 비트 16-22 unmask. 실행 중 mgmt 링을 읽어 `XC5 WLBT: mgmt message: channel ...`
  (채널 0, 메시지 0 = MM_START_IND), mxlog 쓰기 인덱스 변화, 끝나면 20초마다 3분간 `XC5 WLBT summary: ...` 반복.
  이미지 `xc5-ios-wifi3-console.tar`(드롭 1794046067 그대로).
- **wifi3 결과(영상 20261107_194332, 가로로 찍힘 → ffmpeg `transpose=2`)**: `XC5 WLBT t+5000/t+15000 ms: STATUS 0x1
  STATES 0x10 ... mbox INTSR0 0x410000 INTMSR0 0x410000 INTSR1 0 ... ISSR0-3 0x2222801b 0x234000 0xbcdeedcb 0`.
  → **펌웨어가 to-AP 비트 0(관리 채널 쓰기)과 6(mxlog 쓰기)을 올림**, ISSR0도 0x22220004 → 0x2222801b로 달라짐(mxconf를 읽고
  더 진행). mxconf는 0x234000. 다른 줄(`mgmt message`, summary)은 콘솔이 너무 빨리 밀려서 못 찾음.
  조치(커밋 bf017de, xnu CI 42365571196): heartbeat 블록에 `XC5 wlbt pwr= cfg= smc= st= issr0= sr0= msr0= mgmt rN wN msgs=
  first=... log wN` 한 줄. 이미지 `xc5-ios-wifi4-console.tar`.
- **wifi4 결과(영상 20261107_195928): Wi-Fi 2단계 성공.** heartbeat: `XC5 wlbt pwr=1 cfg=1 smc=0 st=0x10 issr0=0x2222001b
  sr0=0x410000 msr0=0x410000 mgmt r1 w1 msgs=1 first=10b10000 ffff0000 ffffffff log w190`. 첫 메시지 바이트 `00 00 b1 10` =
  채널 0(MAXWELL_MANAGEMENT), 메시지 0 = **MM_START_IND, arg 0x10b1** → 펌웨어가 정상 시작을 알림(삼성 드라이버가
  기다리는 바로 그 메시지). CFG_REQ 보임, TZPC SMC 0(성공), mxlog 190 패킷. 그 뒤 부팅·잠금화면 정상.
- **다음 = Wi-Fi 3단계**(큼): (a) mxlog 해독(펌웨어 로그를 콘솔에, 디버깅용), (b) 서비스 관리 채널(채널 1, srvman:
  WLAN 서비스 열기 `scsc_mx_service_open`/`scsc_mx_service_start`), (c) HIP4(데이터 경로: 공유 메모리 큐, mbulk) 초기화,
  (d) MLME로 MAC 주소 받기·검색(scan) → 그다음 iOS 쪽(en0 + IO80211 흉내 + apple80211 ioctl, y2sios/Scion 방식).
  참고 소스(같은 A21s 커널): `drivers/misc/samsung/scsc/srvman*.c`, `scsc_service.c`, `drivers/net/wireless/scsc/`(hip4.c,
  mlme.c, mgt.c, fapi.h).
- **Wi-Fi 3a(커밋 6ebf421, xnu CI 42366767227)**: MM_START_IND를 받으면 채널 1(SERVICE_MANAGEMENT)로
  `sm_msg_packet { service 0 (NULL), SM_MSG_START_REQ 0, optional_data 0 }`(packed 6 B, servman_messages.h)을 from-AP 링에 쓰고
  write bit 2를 INTGR1로 울림(삼성 client_test.c가 NULL 서비스를 이렇게 시작). 기대 응답: 채널 1, `{0, SM_MSG_START_CFM 1}` →
  heartbeat `last=..010001`(바이트 01 00 01 ..). heartbeat 줄 형식: `XC5 wlbt ... msgs=N first=.. last=.. .. sent=N out rN wN log wN`.
  이미지 `xc5-ios-wifi5-console.tar`. 그다음 3b: WLAN 서비스(1)는 HIP4 설정 구조의 ref가 필요(`drivers/net/wireless/scsc/hip4.c`
  hip4_init → scsc_mx_service_start(service, hip control ref)).
- **wifi5 결과(영상 20261107_201554): 3a 성공.** `XC5 wlbt pwr=1 cfg=1 smc=0 st=0x10 issr0=0x2222001b sr0=0x430000 mgmt r2 w2
  msgs=2 first=10b10000 last=00010001 ff000000 sent=1 out r1 w1 log w198` → 두 번째 메시지 바이트 `01 00 01 00` = 채널 1,
  서비스 0, **SM_MSG_START_CFM** (NULL 서비스 시작 확인). 우리 요청을 펌웨어가 읽음(out r1 w1, to-AP 비트 1). 즉 AP↔펌웨어 관리
  채널 양방향 동작.
- **Wi-Fi 3b(커밋 575658d, xnu CI 42368012766)**: MM_START_IND 뒤 NULL 대신 **WLAN 서비스(1)** 시작. 공유 메모리 +4 MiB
  (`HIP_OFF 0x400000`, 별도 4 MiB 매핑)에 hip4_hip_control(hip4.h 레이아웃): init {0xcaaa0400, conf_hip4_ver(펌웨어가 씀),
  v4 ref 0x4000a0, v5 ref 0x400020} · config v5(120 B, @32) · v4(227 B, @160), 둘 다 magic 0xcaba0401, cache line 64,
  host_buf(TX) 0x40a000 1.06 MiB, fw_buf(RX) 0x51a000 1 MiB, MIB @+0x2000 = 스톡 `mx140_wlan.hcf`+`mx140_wlan_sw.hcf`
  (드롭 `wifi/wlan.hcf`, `wifi/wlan_sw.hcf`, wlbtfw.sh가 만듦), scoreboard @448, 큐 6개×(u32[256]+인덱스 3 B, 1088 B) @1472,
  비트 to-AP 7(INTMR0 unmask) / AP->R4 5. 그 뒤 `SM_MSG_START_REQ {1, 0, 0x400000}`. `xc5wlan=0`이면 3a(NULL).
  heartbeat 새 줄: `XC5 hip cfm=<START_CFM 받은 서비스 비트마스크, WLAN이면 0x2> ver=<펌웨어가 고른 HIP 버전 4/5> compat= mlme=
  ma= build= patch= room=head/tail scb=.. q0 r/w q3 r/w mib=4496`. 기대: cfm=0x2, ver≠0, build≠0. 이미지 `xc5-ios-wifi6-console.tar`
  (76,083,200 B, 드롭 패키지 1794051101: hcf 두 개 추가; 인자 `xc5gpu=4 xc5gl=2 xc5wall=1 xc5metal=1 xc5ckpt=1 xc5wifi=1`).
- **wifi6 결과(영상 20261107_203333): 패닉.** MM_START_IND 직후 `kmem_alloc(..., 4194304, 0x100...) failed with 3 @vm_kern.c:178`
  → 두 번째 4 MiB `ml_io_map_wcomb`도 커널 맵에 자리가 없음(wifi1과 같은 종류). 조치(커밋 027a3b6): HIP를 이미 매핑된 앞
  4 MiB 안 `HIP_OFF 0x240000`(mxconf 힙 끝 뒤)으로 옮김 → 제어 0x240000, MIB 0x242000, TX 0x24a000, RX 0x35a000-0x45a000
  (RX 뒷부분은 매핑 밖, 펌웨어가 쓰는 곳이라 지금은 상관없음; 나중에 작은 매핑으로 읽기). START_REQ ref = 0x240000.
  이미지 `xc5-ios-wifi7-console.tar`(같은 인자, 같은 드롭).
- **wifi7 결과(영상 20261107_204718): 패닉 없음, 부팅·잠금화면 정상.** `XC5 hip cfm=0 ver=4 compat=0 mlme=0xe07 ma=0x..
  build=0 patch=0 room=248/26 scb=00000000 q0 0/0 q3 0/0 mib=0`, `XC5 wlbt ... mgmt r1 w1 msgs=1 last=10b10000 sent=1 out r1 ...`.
  → **펌웨어가 HIP4 설정을 읽고 v4를 골라 자기 칸을 채움**(conf_hip4_ver=4, SAP_MLME 0xe07, MA-UNITDATA headroom 248 /
  tailroom 26). START_REQ도 읽음(out r1). 하지만 **START_CFM은 아직 없음**(msgs=1), 그리고 **mib=0**: hcf 두 파일을 못 읽음.
  wifi6이 드롭 설치 전에 패닉 → 새 드롭(hcf 포함)은 이번 부팅에 설치됐는데(`drop state=3 written=27`), make_hip은 그보다
  먼저 돌았을 가능성이 큼. 다음: 같은 이미지로 **재부팅만** 해서 mib=4496인지, 그때 cfm=0x2가 오는지 확인.
  (안 오면: MIB 없이 펌웨어가 WLAN 시작을 보류/실패하는지 mxlog 해독이 필요.)
- **wifi7 재부팅(영상 20261107_205202)**: 결과 같음. `XC5 hip cfm=0 ver=4 compat=0 mlme=0xe07`, `last=10b10000 ... sent=1
  out r1 log w194`가 끝까지 그대로 → START_CFM 없음, 펌웨어 로그가 194에서 멈춤(NULL 서비스 때는 198까지 감). mib 값은
  줄이 화면 밖으로 잘려 못 읽음. 추정: 펌웨어가 WLAN 시작 중 멈추거나 실패.
- **조치(커밋 4443e04)**: (1) hcf를 최대 90초 기다림. (2) **mxlog 해독**: 같은 빌드(10344, 2024-07-03)의
  `vendor/etc/wifi/mx140/debug/common/log-strings.bin`(867 KB, 드롭 `wifi/log-strings.bin`, wlbtfw.sh)을 커널이 읽고, 링의
  레코드(헤더 sync 0x9669=phase 5: 길이 워드 수, timestamp, 문자열 오프셋, 32비트 인자; 0xA55A=phase 4 문자 그대로)를 풀어
  콘솔 `XC5 FW n: <timestamp> <문장>`, heartbeat에 마지막 5줄 `XC5 fw<n> ...`. 스레드는 0.5초마다 계속 로그·관리 링을 읽음.
  (3) heartbeat 줄 `XC5 hip mib=.. cfm=.. ver=.. mlme=.. build=.. scb=.. q0 r/w q3 r/w ls=<log-strings 크기> log <레코드 수> bad <깨진 헤더>`,
  펌웨어 panic record가 0이 아니면 `XC5 wlbt PANIC ...`. 이미지 `xc5-ios-wifi8-console.tar`(76,953,600 B, xnu CI 42370479102, 드롭 패키지 1794053226 = log-strings 포함, 인자 wifi7과 같음).
  첫 부팅에서 드롭 설치가 60초보다 늦으면 로그가 해독 없이(`fmt@오프셋`) 나옴 → 그럴 땐 재부팅 한 번.
- **wifi8 결과(영상 20261107_210843): 로그 해독 성공.** `XC5 hip mib=4496 cfm=0 ver=4 mlme=0xe07 ... ls=866969 log 56 bad 0`.
  펌웨어 로그 끝부분: `APM IPC -- CMD 17..21 VALID !`, `apm_proto_check_valid_ack()`, `WLBT/FM LDOs(LDO14, 18, 19, 20)`,
  `FM=0 Command sequence completed`, **`FW 56: 1a9c8 Received SM_MSG_START_REQ for service: 1`** → 그 뒤 아무 로그 없음, CFM 없음,
  PANIC 줄 못 봄. 원인 후보: 삼성 드라이버는 hcf를 그대로 주지 않음(mgt.c slsi_mib_open_file: 8바이트 헤더 버림,
  slsi_mib_get_platform: 앞의 0xFFFE/0xFFFF 키(플랫폼 이름 `NACHO_S612_XCOVER5_KOR`) 버림). 우리는 파일 전체를 줌.
  조치(커밋 ac7babe, xnu CI 42371495675): `hcf_payload()`로 같은 방식으로 자름 → wlan.hcf 4082 B + wlan_sw.hcf 268 B = mib 4350. 콘솔
  `XC5 WLBT: MIB payloads ...`. 이미지 `xc5-ios-wifi9-console.tar`(드롭 그대로 1794053226).
  그래도 멈추면: 첫 테스트 = WLAN 요청 뒤 NULL 서비스 START_REQ도 보내 관리 태스크가 살아 있는지 보기.
- **wifi9 결과(영상 20261107_212337): Wi-Fi 3b 성공 — WLAN 서비스 시작됨.** `XC5 hip mib=4350 cfm=0x2 ver=4 mlme=0xe07`,
  `XC5 wlbt ... mgmt r2 w2 msgs=2 ... last=00010101 ff000000 sent=1` (채널 1, 서비스 1, SM_MSG_START_CFM). 펌웨어 로그:
  `Received SM_MSG_START_REQ for service: 1` → `fw57 627b5 Sending SM_MSG_START_CFM for a service: 1` → `fw58 Speedy drive
  strength changed from 1 to 2`. INTSR0 0x480000(to-AP 비트 3, 6). 부팅·잠금화면 정상. **원인은 MIB 형식**(hcf 헤더+플랫폼 키를
  떼야 함). scoreboard/큐는 아직 0(호스트가 아무 신호도 안 보냈으니 정상).
- **다음 = Wi-Fi 3c**: HIP4 데이터 경로로 첫 FAPI 신호 주고받기. 삼성 순서(hip4_setup, mgt.c slsi_start →
  slsi_mib_initial_get): (1) to-AP 비트 7 인터럽트·scoreboard·큐 6개 읽기(FH ctrl/data/RFB, TH ctrl/data/RFB), (2) mbulk 풀
  (TX ctrl 64 KiB)로 MLME-GET.request(MIB에서 MAC 주소 등 읽기)를 TH? 아님 FH 큐에 넣고 AP->R4 비트 5 울리기, (3) 응답
  MLME-GET.confirm을 RX에서 읽어 MAC 주소 출력. 자료: hip4.c(hip4_q_add_signal, scoreboard 인덱스), mbulk.h, fapi.h, mib.c.
- **Wi-Fi 3c 구현**: MAC 주소는 펌웨어 것이 아님(삼성 드라이버가 efs 파일에서 읽어 줌) → 첫 신호는 드라이버의
  slsi_mib_initial_get처럼 **MLME-GET.request(0x2001)**: MIB 키 unifiChipVersion 0x07E6, unifiHardwarePlatform 0x0941,
  unifiFirmwareBuildId 0x07E5 (`{u16 psid, u16 0}`씩). 큐: 0 FH ctrl, 1 FH data, 2 FH RFB, 3 TH ctrl, 4 TH data, 5 TH RFB;
  scoreboard 바이트 위치 [쓰기, 읽기] = {0,64} {1,65} {66,2} {67,3} {68,4} {5,69}(펌웨어 쪽 +64). mbulk = 20바이트 헤더 {next,
  flag(1 신호|4 상대 소유), class 5, pool 1, refcnt, dat_bufsz, sig_bufsz, len, head, chain} + 신호 버퍼(펌웨어 헤더 4 B + FAPI
  헤더 {id, receiver 0, sender 0xC001, fw_ref} + vif + spare 12) + headroom(248) 뒤 데이터. TX ctrl 영역 첫 2 KiB 슬롯
  (공유 메모리 0x34a000)에 만들어 q0에 넣고 AP->R4 비트 5. q3에서 응답(MLME-GET.confirm 0x2101)을
  읽고 q5로 돌려줌, q2 따라감. RX 512 KiB(0x35a000-0x3da000, 매핑 안). heartbeat: `XC5 hip ... | q0 wN rN q2 wN q3 wN rN q5 wN rN`,
  `XC5 mlme sent= got= id= siglen= len= data ...`. 결과는 아래 wifi10. 이미지 `xc5-ios-wifi10-console.tar`(커밋 920f99a, xnu CI 42372891680, 드롭 그대로 1794053226).
- **wifi10 결과(영상 20261107_214038): HIP4 왕복 성공.** `XC5 hip mib=4350 cfm=0x2 ver=4 ... | q0 w1 r1 q2 w1 q3 w8 r8 q5 w8 r8`,
  `XC5 mlme sent=1 got=8 id=0x2101 siglen=24 len=(화면 밖에서 잘림) data 00000000 ...`, sr0 0xc30000(to-AP 비트 7 = HIP).
  → 펌웨어가 우리 요청을 가져가고(q0 r1) 버퍼를 돌려주고(q2 w1) TH ctrl로 신호 8개를 보냄, 마지막이 **MLME-GET.confirm**.
  데이터 6워드는 0 → len이 0이거나 데이터가 체인 mbulk에 있음(미확인). 조치(커밋 2585702): 짧은 줄로 `XC5 mlme sent= got= len=
  sig=`, `XC5 ids <마지막 신호 8개 id>`, `XC5 mb <confirm mbulk 헤더 5워드>`, `XC5 md <데이터 6워드>`, `XC5 mc <체인 다음 세그먼트
  데이터>`. 이미지 `xc5-ios-wifi11-console.tar`(xnu CI 42373806596, 드롭 그대로).
- **wifi11 결과(영상 20261107_215418): Wi-Fi 3c 성공 — 펌웨어가 MIB 값을 돌려줌.** `XC5 mb 00000000 010001c5 001c0000
  001c0000 00392000` = confirm mbulk: flag 0xc5(신호|상대 소유|CHAIN_HEAD|CHAIN), class 1(HOSTIO), sig_bufsz 28, len 0, head 28,
  **chain_next 0x392000** → 데이터는 체인 다음 세그먼트에 있음(md 0인 이유). `XC5 mc 000307e6 0000a782 00020941 07e57781` =
  MIB 응답 `{psid 0x07E6 len 3: 82 a7 00}` → **unifiChipVersion 0xA700**, `{0x0941 len 2: 81 77}` → **unifiHardwarePlatform
  0x77**, 그다음 `{0x07E5 ...}`(FirmwareBuildId, 16바이트에서 잘림). MIB 정수 = 첫 바이트 0x80|길이, 그 뒤 빅엔디언(mib.c
  slsi_mib_decode_uint32). 즉 **FAPI MLME 요청/응답이 HIP4로 완전히 동작**.
- **다음 = Wi-Fi 3d(검색)**: 삼성 순서대로 MLME-ADD-VIF.request(가상 인터페이스, MAC 주소는 우리가 정함 — efs 대신 고정/임의
  로컬 주소) → MLME-ADD-SCAN.request → MLME-SCAN.indication(찾은 AP 비콘/프로브 응답, 데이터 큐 또는 ctrl) →
  MLME-SCAN-DONE.indication. 응답 데이터는 체인 mbulk까지 따라가 읽어야 함. 그다음 iOS 쪽(en0, apple80211).
- **Wi-Fi 3d 구현(커밋 130c0db, xnu CI 42374958927)**: GET confirm 뒤 **MLME-ADD-VIF.request 0x2007** {vif 1, 주소 02:58:43:35:00:01(로컬), type 2
  station, device 주소 같음} → confirm 0x2107 result 0이면 **MLME-ADD-SCAN.request 0x2005** {vif 0, scan_id 0x100, type 2 full,
  device 주소, report_mode 4 real-time}, 데이터 = SSID 원소 길이 0 + 스캔 타이밍 원소(dd 11 00 16 32 01 01, 0) + 채널 목록 원소
  (dd 2c 00 16 32 01 02 + 2.4 GHz 1-13채널 {u16 2×MHz, u8 0}). MLME-SCAN.indication 0x2300 {vif, channel_frequency 2×MHz,
  rssi}의 데이터 = 비콘/프로브 응답 프레임(체인 mbulk까지 모음) → SSID(24+12바이트 뒤 원소 0), BSSID(+16). 6개까지 기억.
  TH data(q4)도 읽음. 송신은 TX ctrl 32칸 순환. heartbeat: `XC5 mlme sent= got= rfb= ids ...`, `XC5 chip a700 plat 77 build N`,
  `XC5 vif <결과> scan <결과> ind <수> done <수> aps <수>`, `XC5 ap <MHz> <dBm> <SSID>`. 이미지 `xc5-ios-wifi12-console.tar`.
- **Wi-Fi 1단계 결과 읽기**(끝남): 콘솔 `XC5 WLBT: firmware ... entry 0x1a9`, `power on`, `CFG_REQ`/`no CFG_REQ in 1 s`,
  `SMC CONN_IF ...`, `configured ... CFG_ACK given`, `XC5 WLBT t+N ms: ...` 줄들, `panic record ...`. 그다음 2단계:
  mxconf + mxmgmt 전송(공유 메모리 링)을 만들어 MM_START_IND 받기(mxman.c transports_init, mxconf.h, mxmgmt_transport.c).
- (끝남) RTC: rtc4에서 날짜 해결.

0. **전원 키**(보류): pmic1 콘솔에서 `XC5 PMIC: ACPM initdata ...`, `channel 2: ...`, `top block register 0 ...`, `pmic 00: ...` 덤프,
   그리고 전원 키를 누를 때 `XC5 PMIC: pmic RR: AA -> BB` 줄. 바뀌는 레지스터/비트를 찾아 `xc5pmic=2 xc5pwrreg=0xRR
   xc5pwrbit=N (xc5pwrlow=1)`로 → keys 드라이버의 Power(비트 5)에 합쳐짐. 채널을 못 찾거나 읽기가 실패(`--`)하면 ACPM 레이아웃
   (initdata 오프셋, 첫 페이지 접근 불가 등)부터 확인. RTC 블록 덤프는 다음 작업(RTC 시간) 재료.

0. 사진 앱 조사(사용자 요청): `xc5-ios-photo1-console.tar` = 부팅 79 기준 + `sweetscan=backboardd,SpringBoard,MobileSlideShow,
   assetsd,photoanalysisd,mediaanalysisd`(이 프로세스들이 SIGABRT/SIGTRAP로 죽으면 커널이 `STR` 줄로 힙 문자열을 찍음).
   사용자가 사진 앱을 열고 콘솔 영상을 보냄 → `exec MobileSlideShow`/`assetsd`, `CRASH ...`, `STR ...` 읽기. 사진 앱은
   `/Applications/MobileSlideShow.app`, 라이브러리 데몬 `AssetsLibrary.framework/Support/assetsd`, 위젯 `PhotosReliveWidget.appex`.
   - 부팅 80 결과: 하얀 화면에서 멈춤, 크래시·STR 없음(자세한 것은 port.md). 다음 이미지 `xc5-ios-photo2-console.tar`
     (xnu CI 42342598615, 위 + `xc5photos=1`): 사진 앱에 `libxc5poster.dylib` → 콘솔 `XC5 PB: MobileSlideShow (pid) ... inserted`,
     `xc5poster: loaded in MobileSlideShow`, 1초마다 `xc5poster: #N pc ... bt ...`(40초). 주소는 `dsc.py`(슬라이드는 이미 뺌 → `dsc.py 0 주소...`)로.
     이 줄이 안 나오면 앱 프로세스가 아예 안 뜬 것(FrontBoard/RunningBoard 쪽).
   - 비슷한 사례(2026-11-07 조사): HakiOS A32(같은 Mali-G52, GL 렌더러)는 사진 앱이 죽음("one corpse: MobileSlideShow",
     `docs/gpu-a32.md`, 원인 미상), CPU 렌더링 10분 사용 기록에는 사진 앱 사용이 문제없이 들어 있음(`stability-a32.md`).
     y2sios: assetsd가 볼륨 속성(getattrlist) 실패로 CacheDelete에서 예외(ramfs.c 주석), photoanalysisd는 계속 죽어서 막음
     (`userland/mkjobs.py` DENY). S9+ 계열 노트: 사진 앱 첫 실행은 DB 스키마·인덱스 생성(CREATE INDEX 10번).
   - **공유 캐시 확인**: 사진 앱 보관함 화면을 그리는 PhotosUICore의 PXG 엔진은 렌더러가 Metal뿐
     (`PXGMetalRenderer`, `PXGMetalRenderDestination`, ...; `tools/xcover5/dev/dsgrep.sh`로 확인). Metal 장치가 없으면 아무것도
     안 그려져 하얀 화면일 가능성이 큼. → `xc5-ios-photo3-console.tar` = photo2 + `xc5metalfor=chronod,MobileSlideShow`
     (커널 재빌드 없음). 사진 앱이 그려지면 확인, 죽으면 `STR`/`metal E` 줄로 원인.
   - **부팅 81 결과: 원인은 Metal이 아니라 checkpointd**. 사진 앱 메인 스레드가 `MCCopyCheckpointValue`(libMobileCheckpoint)로
     `checkpointd`의 XPC 답을 계속 기다림(port.md 81). 다음 이미지 `xc5-ios-photo4-console.tar`(xnu CI 42344322122,
     mesa-ios CI 42344322131, 드롭 1794031007): checkpointd 모든 스레드 3초마다 `xc5poster: #N tK pc ... bt ...`,
     `sweetscan`에 checkpointd·mediaserverd. 볼 것: checkpointd가 뜨는지(`XC5 PB: checkpointd ... inserted`), 어디서 막히는지
     (FigCheckpointSupport → mediaserverd? FairPlay?), 죽는지. 우회 후보: 막는 쪽 고치기, 또는 Checkpoint.plist 미리 두기.
   - **부팅 82 결과: checkpointd가 FairPlay(fairplayd.H2) MIG 호출에서 영원히 기다림**(port.md 82). S9+/y2sios의 App Store와
     같은 모양(fairplayd.H2는 시작하자마자 exit 0). 다음 이미지 `xc5-ios-photo5-console.tar`(xnu CI 42345626657, mesa-ios CI
     42345626740, 드롭 1794032165) = photo4 + `xc5ckpt=1`: checkpointd에 `libxc5ckpt.dylib`(mach_msg 수신 3초 제한).
     볼 것: `xc5ckpt: loaded in checkpointd`, `xc5ckpt: ... mach_msg id ... limited`, `answered 0x10004003 (no reply in time)`,
     그 뒤 사진 앱이 그려지는지. 사진 앱 다음 벽은 Metal일 수 있음(PXG는 Metal 렌더러만, `xc5metalfor`에 MobileSlideShow 넣어 둠).
     같은 FairPlay 대기는 App Store 등 다른 곳에도 있을 것(S9+는 AMS 메서드 교체로 해결).
   - 부팅 83: 라이브러리는 들어갔지만 제한 줄이 안 나옴(port.md 83). 다음 `xc5-ios-photo6-console.tar`(xnu CI 42347265756,
     mesa-ios CI 42347265677, 드롭 1794033462): checkpointd에 libxc5ckpt + libxc5poster(모든 스레드 3초마다), mach_msg 처음
     12번 모두 `xc5ckpt: mach_msg #N option ... id ...`. 볼 것: mach_msg 줄이 나오는지(안 나오면 interpose 미적용 →
     다른 방법: fishhook식 GOT 덮어쓰기), 스레드가 어디서 멈췄는지.
   - 참고 저장소: Scion-Project/scion-kernel(HakiOS 계열 S9 포트, 2026-11-06 커밋 7359383; 앞서 본 ios-s9plus-exynos와 같은 줄기)과
     scion-installer(TWRP 설치 zip 빌더). `docs/ru/notes.md` 2007-2235: fairplayd.H2는 거기서도 **해결 안 됨**(10 ms 만에 exit 0,
     IOKit도 안 부름, FairPlayIOKit 드라이버 스텁은 소용없음 판명). 대처는 클라이언트에서 FairPlay 질문에 대신 답하기.
     checkpointd·사진 앱 기록은 없음 → 우리 checkpointd 시간 제한 방식이 같은 방향.
   - herolte(wardevsa/ios-a5xelte 브랜치 herolte, S7): `tools/herolte-disabled.list` 맨 위가 `com.apple.fairplayd.H2`,
     `gpu/eagl/test/hero-run.c`가 root로 `/private/var/db/com.apple.xpc.launchd/disabled.plist`를 씀(CPU 낭비 때문).
     fairplayd.H2 job은 KeepAlive이고 포트가 `com.apple.fairplayd`, `.versioned`, **host special port 17 `com.apple.unfreed`**
     (FairPlay 클라이언트는 이걸로 찾음). 끄면 포트가 없어 클라이언트가 바로 실패함.
     → (커밋 4766ad4) 드롭 패키지에 `/private/var/xc5/launchd-disabled.plist`가 있으면 커널이 launchd의 disabled.plist로 복사
     (원본은 한 번만 `/private/var/xc5/launchd-disabled.orig.plist`로 보관, 없었으면 빈 파일). 내용은
     `tools/xcover5/launchd-disabled.plist`(fairplayd.H2만). **다음 부팅부터** 적용. 되돌리려면 orig 내용을 같은 이름으로 다시 패키징.
     이미지 `xc5-ios-photo7-console.tar`(xnu CI 42348421220, 드롭 1794034425 = photo6 + 이 plist): **두 번 부팅**해야 함
     (첫 부팅 설치 `XC5 DROP: .../disabled.plist: 788 bytes ... (next boot)`, 두 번째 부팅에 fairplayd 없음 → `exec fairplayd.H2`
     줄이 사라져야 함). xc5ckpt·샘플러도 그대로 들어 있음.
   - **부팅 84 결과(photo7): 사진 앱이 checkpointd를 넘어감**, 우리 Metal 번들을 불러 그리기 시작하다가
     `assetsDataSourceManager.dataSource.numberOfSections == 1` 단언으로 abort(보관함 데이터가 비어 있음, port.md 84).
     다음 `xc5-ios-photo8-console.tar`(xnu CI 42349821373, mesa-ios CI 42349821374, 드롭 1794035482; 한 번 부팅이면 됨,
     disabled.plist는 이미 들어가 있음): assetsd의 모든 스레드도 3초마다 `xc5poster: #N tK ...`. 볼 것: `XC5 PB: assetsd ... inserted`,
     assetsd가 어디서 기다리는지(라이브러리 열기, 마이그레이션, DB 생성), `CRASH assetsd`/`STR`.
1. M4 위젯: chronod가 Metal로 그리는 것이 무엇인지(어떤 위젯/요소) 확인. 사진 위젯은 사진 앱(assetsd/photolibraryd)부터. 위젯 확장이 스냅샷을 못 주는지, chronod가
   그리다 막히는지. 다음 영상에서 홈 화면 오래 두고 `CRASH`, `metal E ...`, `chronod` 줄 확인. 필요하면 확장 프로세스를
   `xc5metalfor`에 추가(쉼표 목록). `gpuAddress 0`(Mesa 버퍼 경로에서 GPU 주소를 안 알려줌)도 확인할 것.
2. Metal 컴퓨트 한 번(셰이더 metallib을 CI의 Xcode `xcrun metal`로 만들어 드롭에 넣고, `xc5_mtl.c`에서 실행·결과 확인).
3. 안정되면 `xc5mtl=1`(시험)은 빼고 `xc5metal=1`만으로 위젯이 되는지 확인.

## 분석 도구 (`tools/xcover5/dev/`)

- `mntsys.sh`: IPSW의 SystemOS/OS를 apfs-fuse로 마운트(WSL `~/xc5/ipsw`).
- `dsc.py SLIDE 주소...`: 콘솔 `CRASH … slide … bt …` 주소를 공유 캐시 함수로. `dsdis.sh 주소 길이`: 디스어셈블.
  `dsstr.py 주소`: 문자열/CFString. 크래시 메시지는 커널이 `STR …` 줄로 힙 문자열을 찍어 줌(예외 이유가 거기 나옴).
- `frames.ps1`: 영상 → 프레임 묶음.
