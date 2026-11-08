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
- **wifi12 결과(영상 20261107_220938): Wi-Fi 3d 성공 — 폰이 실제 AP를 찾음.** `XC5 chip a700 plat 77 build 498375851`
  (펌웨어 빌드 문자열의 `univ3 498375851`과 같음), `XC5 vif 0 scan 0 ind 1 done 1 aps 1`, `XC5 ap 2412 -57 <근처 AP SSID>`
  (채널 1, -57 dBm). `XC5 mlme sent=3 got=77 rfb=3 ids 8301 8301 8301 2301`, q3/q5 w77 r77. ADD_VIF·ADD_SCAN 모두 result 0,
  SCAN_DONE 1번. 신호 대부분 0x8301(DEBUG SAP 쪽 표시, 미해독). 검색 결과(SCAN_IND)는 1개뿐 → 다른 AP가 없었거나 일부만 보고됨
  (다음에 확인: 0x8301 해독, 활성 검색에 probe 응답, 5 GHz 채널 추가).
- **다음 = iOS 쪽 연결(4단계)**: 커널 안 Wi-Fi 드라이버(xc5_wlbt)를 iOS 네트워크 스택에 붙이기. 후보 순서: (a) 연결
  (MLME-CONNECT.request, 개방형 또는 WPA2 → 4-way handshake는 펌웨어 오프로드 여부 확인), (b) 데이터 경로(MA-UNITDATA,
  TH/FH data 큐)로 이더넷 프레임 주고받기, (c) BSD `ifnet`(en0) 등록 + DHCP, (d) apple80211/IO80211 흉내(설정 앱 Wi-Fi 목록용,
  y2sios/Scion 방식 조사). 가장 짧은 길: 개방형 또는 WPA2 AP에 커널에서 직접 붙고 en0만 올려 ping.
- **4a/4b 구현(커밋 d60a7e9, xnu CI 42376364254, 사용자 선택: 개방형 핫스팟으로 먼저)**: SCAN_DONE 뒤 AP 선택 = 부팅 인자 `xc5ssid=이름`(공백 없는 SSID),
  없으면 privacy 비트 없는(개방형) AP 중 가장 센 것. **MLME-CONNECT.request 0x200b** {vif 1, bssid, auth 0 open, 2×MHz} + SSID 원소
  → CONNECT_CFM 0x210b → (인증·연결) **MLME-CONNECT.indication 0x2303** result 0이면 **MLME-CONNECT.response 0x2200**(vif 1).
  802.1X 포트는 호스트 상태뿐(보낼 것 없음). 데이터: **MA-UNITDATA.request 0x1000** {vif 1, host_tag, priority 0, peer_index 1
  (STA의 AP = queue set 0 + 1), data_unit_descriptor 1 (802.3), bulk 0} + 이더넷 프레임, data 풀 mbulk(class 4, pool 0, TX 데이터
  영역 64칸)로 **FH data 큐(q1)**. 받기: MA-UNITDATA.indication 0x1300 {..., proprieraty_information_length @+20 만큼 건너뜀}.
  확인용으로 커널이 DHCP를 직접 함(DISCOVER 4초마다 → OFFER → REQUEST → ACK). heartbeat: `XC5 ap <MHz> <dBm> o|s <SSID>`,
  `XC5 join <SSID> cfm <x> ind <x> disc <reason>`, `XC5 data tx N cfm x rx N dhcp <0-3> offer a.b.c.d`, `XC5 ip a.b.c.d gw ...`.
  이미지 `xc5-ios-wifi13-console.tar`. 시험: 다른 폰 핫스팟을 **비밀번호 없이, 2.4 GHz**로.
- **wifi13 결과(영상 20261107_225709, 핫스팟 = 개방형·2.4 GHz)**: `XC5 vif 0 scan 0 ind 5 done 1 aps 3`, `XC5 ap 2412 -77 s KT_GiGA_2G_..`,
  `2412 -64 s olleh_WiFi_..`, `2437 -18 o S24` → `XC5 join S24 cfm 0 ind 8212 disc ffff`, data tx 0 dhcp 0. **CONNECT_IND 0x8212 =
  ASSOC_FAILED_CODE(0x8200) + 802.11 상태 18**(AP의 기본 rate 집합을 다 지원하지 않음 → 연결 거부). 인증은 통과, association에서 거부.
  추정: S24 핫스팟이 기본 rate에 BSS membership selector(HT/HE PHY 필수 등)를 넣었거나 펌웨어의 HT가 꺼져 있음. 조치(커밋 5c5a1e8, xnu CI 42379736030):
  진단 출력 — `XC5 rates <개수> caps <1 HT|2 VHT|4 HE>: <rate 바이트들>`(0x80 비트 = 기본 rate, 0xff/0xfe/0xfa 등 = selector),
  `XC5 fw ht <unifiHtActivated> vht <..> rates <unifiSupportedDataRates>`, `XC5 assoc <길이>: <CONNECT_IND 데이터 앞 16바이트>`.
  이미지 `xc5-ios-wifi14-console.tar`. 같은 이미지로 핫스팟 "Wi-Fi 6" 옵션을 끄고 재부팅해 비교하면 좋음.
- **wifi14 결과(영상 20261107_231043)**: `XC5 rates 12 caps 3: 82 84 8b 96 c 12 18 24 30 48 60 6c`(AP: 기본 1/2/5.5/11, 나머지 6-54,
  HT+VHT, selector 없음 → Wi-Fi 6 가설 아님), `XC5 fw ht 1 vht 1 rates 04020ca1 16120c0b ..`(펌웨어: 02 04 0b 0c 12 16 18 24 30 48..
  = AP 기본 rate 다 있음), `XC5 assoc 46: 1000 3a01 0258 4335 0001 cab0 c81e 6a24`(= AP의 Association Response, 우리 주소로).
  → 거부는 AP가 우리 **association request**를 보고 한 것. 조치(커밋 아래): MLME-PROCEDURE-STARTED.ind 0x230a 데이터(= 펌웨어가 보낸
  assoc request 프레임, rx.c slsi_peer_update_assoc_req)의 원소 id와 rate 표시, AP 응답 status(+26) 표시, status 18이면 한 번 더:
  **MLME-ADD-INFO-ELEMENTS.request 0x2004**(vif 1, purpose 0x0010 assoc req) + AP의 rate 원소(1, 50) → CONFIRM 0x2104 → CONNECT 다시. (커밋 b131bab, xnu CI 42380750766)
  heartbeat `XC5 req <길이> ids <n>: ...`, `XC5 req rates <n>: ...`, `XC5 resp <길이> status <AP status> try <0-2> info <결과>`.
  이미지 `xc5-ios-wifi15-console.tar`.
- **wifi15 결과(영상 20261107_232547)**: 하트비트 블록이 ~25줄로 길어져 앞줄(선택한 AP, 요청, 결과)이 촬영 전에 화면 밖으로 밀림 →
  읽을 수 있었던 것은 `XC5 req rates 0 ...`, `XC5 resp 0 status 65535 try 0 info ffff`뿐(그 시점엔 연결 과정 시작 전 또는 AP 못 찾음,
  미확정). 조치(커밋 d0173e2, xnu CI 42381910066): Wi-Fi 하트비트를 6줄로 줄이고 중요한 줄을 맨 뒤로: `XC5 wlbt pwr st issr0 mgmt log fwN: <마지막 펌웨어
  로그>`, `XC5 w cfm vif scan ind done aps got`, `XC5 w ap <SSID> <MHz> <dBm> o|s`(선택 전엔 `ap?` 3개), `XC5 w req <길이> ids ... r
  <rate 8개>`, `XC5 w join cfm ind st <AP status> try info disc`, `XC5 w data tx cfm rx dhcp offer ip`. 연결할 AP가 없으면 8초 뒤 재검색.
  이미지 `xc5-ios-wifi16-console.tar`.
- **wifi16 결과(영상 20261107_233929, 짧은 하트비트로 읽힘)**: `XC5 w ap S24 2437 -22 o`, `XC5 w req 114 ids 0 1 24 21 2d 46 r c 12 18
  24 30 48 60 6c`, `XC5 w join cfm 1 ind ffff st 18 try 2 info 0`. → 펌웨어의 association request rate가 **6-54 Mb/s(OFDM)뿐, 1/2/5.5/11
  없음**(P2P 방식) → AP 기본 rate(1/2/5.5/11) 불충족 → 상태 18. 재시도(ADD_INFO_ELEMENTS 0, CONNECT cfm 1)는 효과 없음.
  **원인: ADD_VIF의 device_address** — 드라이버는 station이면 FF:FF:FF:FF:FF:FF(cfg80211_ops.c slsi_connect, P2P일 때만 실제 주소),
  우리는 우리 주소를 넣음. 조치(커밋 f459cfa, xnu CI 42383026520): FF로. rate 재시도는 `xc5wretry=1`일 때만. 이미지 `xc5-ios-wifi17-console.tar`.
- **wifi17 결과(영상 20261107_235543): Wi-Fi 4a/4b 성공 — 개방형 핫스팟에 연결하고 DHCP로 IP를 받음.**
  `XC5 w cfm 2 vif 0 scan 0 ind 2 done 1 aps 1 got 147`, `XC5 w ap S24 2437 -20 o`, `XC5 w req 120 ids 0 1 32 24 21 2d r 82 84 8b 96
  c 12 18 24`(이제 11b rate 들어감 + 확장 rate 원소 0x32), `XC5 w join cfm 0 ind 0 st 0 try 0`, `XC5 w data tx 2 cfm ffff rx 7 dhcp 3
  offer 10.168.19.211 ip 10.168.19.211`. → ADD_VIF device_address = FF가 원인이었음. 프레임 송신(DISCOVER, REQUEST)과 수신(OFFER, ACK
  포함 7개) 모두 동작. MA-UNITDATA.confirm은 안 옴(cfm ffff, 요청 안 하면 안 오는 듯 — 문제 아님).
- **다음 = 4c: iOS 네트워크에 en0 올리기.** 커널에서 BSD `ifnet` 생성(ifnet_allocate_extended, family IFNET_FAMILY_ETHERNET,
  ifnet_attach, ether 주소 02:58:43:35:00:01) + output 콜백 → MA-UNITDATA, 수신 프레임 → ifnet_input. 주소는 DHCP로 받은 값을
  커널에서 설정(SIOCAIFADDR, 서브넷·게이트웨이·DNS는 ACK에서 — 현재 mask/gw/dns도 파싱함), 기본 경로 추가. 확인: 다른 기기에서
  ping, 또는 iOS 앱의 인터넷. 그다음 configd/IPConfiguration이 en0을 인식하게(IOKit IONetworkInterface 흉내) 할지 결정.
- **4c 구현(커밋 c7a98de, xnu CI 42384527367)**: DHCP ACK 뒤 커널이 레거시 BSD 이더넷 인터페이스를 만듦(if_fake.c BSD 모드 방식):
  `ifnet_allocate_extended`(IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO, name "en", 비어 있는 첫 unit — 보통 en0, family ETHERNET,
  IFT_ETHER, ether_demux/add_proto/del_proto/check_multi/frameout_extended), attach, lladdr 02:58:43:35:00:01, UP|RUNNING.
  출력 콜백 → MA-UNITDATA(send_frame). 입력: 받은 모든 프레임 → mbuf(헤더 포인터 = 이더넷 헤더, data는 그 뒤) → 락 밖에서
  `ifnet_input`. HIP4 큐는 뮤텍스 하나로 보호(스레드와 출력 경로), 스레드는 10 ms마다 폴링. 주소: 커널 소켓 SIOCAIFADDR(ACK의
  주소·마스크), 기본 경로: 라우팅 소켓 RTM_ADD(게이트웨이), 확인: ICMP 데이터그램 소켓으로 2초마다 게이트웨이 ping.
  heartbeat 마지막 줄 `XC5 w if <이름> <err> addr <err> route <err> out N in N drop N ping <응답>/<보냄> (<err>)`.
  이미지 `xc5-ios-wifi18-console.tar`. DNS는 아직 없음(IP 통신만).
- **wifi18 결과(영상 20261108_001306): Wi-Fi 4c 성공 — iOS 네트워크 스택이 Wi-Fi로 통신.** `XC5 w if en0 0 addr 0 route 0 out 6 in 5
  drop 0 ping 3/3 (0)` → 이후 `out 11 in 10 drop 0 ping 8/8 (0)`, `XC5 w data tx 13 rx 12 dhcp 3 ... ip 10.168.19.211`. en0 생성·주소·
  기본 경로 모두 성공, 게이트웨이 ping 100% 응답(ICMP 데이터그램 소켓 → TCP/IP 스택 → en0 → MA-UNITDATA → 핫스팟 → 다시 en0 →
  ifnet_input → 소켓). 사용자: 핫스팟 폰에도 연결 기록이 남음. 부팅·잠금화면 정상.
- **다음 후보**: (a) DNS(configd에 resolver 등록: SCDynamicStore `State:/Network/Service/<id>/DNS` + Global/IPv4 PrimaryInterface,
  또는 /etc/resolv.conf 흉내) → Safari 등 앱 인터넷, (b) WPA2(커널 PSK·4-way handshake + MLME-SETKEYS), (c) 설정 앱 Wi-Fi 목록용
  IO80211/apple80211 흉내, (d) 수신 인터럽트(10 ms 폴링 대신 to-AP 비트 7), (e) 핫스팟 SSID/비밀번호를 드롭 파일로.
- **4d 구현(커밋 f34c966, xnu CI 42385981516, mesa-ios CI 42385981442)**: 커널이 DHCP 결과를 sysctl `kern.xc5wifi`("up <if> <주소>
  <마스크> <라우터> <dns>")로 내보내고, `kern.xc5netstat`(쓰기 가능 int)로 결과를 받음. **libxc5net.dylib**(gpu/eagl/src/xc5_net.c)이
  `xc5net=1`로 configd에 주입(xc5_drop.c 대상 `/usr/libexec/configd`) → kern.xc5wifi를 기다렸다가 SCDynamicStore에
  `State:/Network/Service/XC5WIFI/IPv4 {Addresses, SubnetMasks, Router, InterfaceName}`, `.../DNS {ServerAddresses}`,
  `State:/Network/Interface/en0/Link {Active}` 씀 → IPMonitor가 순위 매김 → `State:/Network/Global/IPv4` PrimaryInterface와
  `State:/Network/Global/DNS` 확인, `getaddrinfo www.apple.com`. 결과 비트: 1 씀, 2 en0 primary, 4 global DNS, 8 이름 풀림,
  0x100+SCError 실패, 0x1ff SC 함수 못 찾음. iOS SDK가 SCDynamicStore API를 iOS 사용 불가로 표시 → dlsym으로 호출.
  콘솔 `XC5GPU client: ... xc5net: ...`, heartbeat `XC5 w net <비트> dns <서버>`. 이미지 `xc5-ios-wifi19-console.tar`(인자 + `xc5net=1`,
  **새 드롭 1794065387**). 첫 부팅은 드롭 설치가 configd 시작보다 늦어 주입 안 될 수 있음 → 한 번 재부팅.
- **wifi19 결과(영상 20261108_003117): Wi-Fi 4d 성공 — 이름 풀이(DNS)까지 동작.** 콘솔: `xc5net: in configd (pid 36), waiting for
  kern.xc5wifi` → `xc5net: en0 10.168.19.211/255.255.255.0 router 10.168.19.1 dns 10.168.19.73` → `service XC5WIFI published` →
  (몇 번 `no global IPv4 yet`) → `global IPv4 primary interface en0` → **`www.apple.com is 23.60.105.29`** → `done, state 0xf`.
  heartbeat `XC5 w net 0xf dns 10.168.19.73`, `ping 10/10`. 즉 configd(IPMonitor)가 en0을 기본 인터페이스로 삼고 DNS 설정을
  resolver에 넘겼고, 시스템 resolver(mDNSResponder 경유 getaddrinfo)가 인터넷 이름을 풀었음. 같은 부팅에서 locationd·
  WirelessRadioManager가 반복 크래시(“Too many corpses”, sig 5) — 네트워크가 생기며 깨어난 듯(Apple Wi-Fi 드라이버/IO80211 없음 추정, 미확인).
- **다음 후보**: Safari 등 앱으로 실제 페이지 열기 확인(사용자), WPA2, 설정 앱 Wi-Fi(IO80211 흉내), WirelessRadioManager/locationd 크래시,
  10 ms 폴링 → 인터럽트, SSID/비밀번호 드롭 파일.
- **사용자(wifi19)**: Safari를 열면 **페이지 영역이 검고, 스크롤하면 흰 화면**이 나옴(UI는 정상). 네트워크인지 그리기(WebKit의 WebContent/GPU
  프로세스 렌더링)인지 가르려고(커밋 f5dfb72, mesa-ios CI 42387543765): libxc5net이 DNS 뒤 **`http://captive.apple.com/hotspot-detect.html`**
  을 TCP로 요청(iOS 자체의 인터넷 확인과 같은 것) → 콘솔 `xc5net: HTTP <바이트> bytes: HTTP/1.x 200 OK, Success`, xc5netstat 비트 0x10.
  성공이면 Safari 검은 화면은 렌더링 문제(다음: Safari 열 때 콘솔에 WebContent/GPU 프로세스 크래시가 있는지 영상으로). 이미지
  `xc5-ios-wifi20-console.tar`(**새 드롭 1794066219**, 인자 wifi19와 같음, 첫 부팅 뒤 재부팅 권장).
- **wifi20 결과(영상 20261108_115727): 인터넷 확정.** `xc5net: www.apple.com is 104.94.216.37` → **`xc5net: HTTP 434 bytes: HTTP/1.0 200 OK,
  Success`** → `done, state 0x1f`. 즉 en0으로 TCP·HTTP가 인터넷까지 오감. 그 뒤 `connectx() - failed necp_set_socket_domain_attributes`
  가 대량 출력 — uipc_socket.c의 printf(연결 대상이 tracker 캐시에 있을 때 도메인 속성 설정 실패, **연결은 계속됨, 무해**), 오히려 앱들이
  이름을 풀고 TCP 연결 중이라는 증거. Safari: apple.com 주소창 정상, 잠깐 **흰 페이지(로딩 막대)가 그려졌다가 검게** 바뀜, 콘솔에
  `SWEET SURF: com.apple.WebKit.GPU create id NN 1125x144 ...`(GPU 프로세스가 IOSurface 생성), `hakios-gl: -attachImage ... not done`.
  → **Safari 검은 화면은 네트워크가 아니라 WebKit GPU 프로세스 렌더링**(추정: GPU 프로세스의 DOM 렌더링/가속 그리기가 Metal을
  요구하는데 Metal은 chronod에만 공개). 시험: 설정 > Safari > 고급 > Feature Flags(실험적 기능)에서 "GPU Process: DOM Rendering"
  (과 Canvas) 끄기 → 다시 Safari. 안 되면 `xc5metalfor`에 com.apple.WebKit.GPU 추가 시험(위험: Metal 실험 단계).
- **사용자: Feature Flags 꺼도 똑같음.** 영상의 콘솔 `XC5 mesa: backboardd: hakios-gl: -attachImage: target 0xde1, texture N, an IOSurface of
  'w30r': not done`가 원인: WebKit GPU 프로세스가 넓은 색역 화면용 **'w30r'**(kCVPixelFormatType_30RGBLEPackedWideGamut, 10비트
  B/G/R XR 384..894, 단일 평면, 불투명)로 페이지 타일을 그리는데 backboardd의 GL 층(gpu/hakios-gl/src/eagl.c my_attachImage)이 'BGRA'·
  'b3a8'만 받음. 조치(커밋 d3c96ad, mesa-ios CI 42442158791): 'b3a8' 경로(deep_to_bgra/bgra_to_deep)를 단일 평면 'w30r'도 받게(투명도 0xff).
  이미지 `xc5-ios-wifi21-console.tar`(**새 드롭 1794108066**, 인자 wifi20과 같음). WSL vhdx 압축 57.8→28.2 GB(사용자, diskpart).
- **사용자: 부팅 중엔 핫스팟의 연결된 기기에 보였는데 부팅 뒤엔 안 보임.** wifi20 마지막(약 3분)에도 `out 2117 in 239 ping 63/63` → 그때까진 연결 유지.
  후보 둘을 고침(커밋 9dc2063, xnu CI 42443761365): (1) TX 데이터 64칸을 순서대로 재사용해 펌웨어가 아직 읽는 칸을 덮어쓸 수 있었음 →
  칸은 FH RFB 큐로 돌아올 때까지 펌웨어 것(비트맵), 0.5초 동안 전부 나가 있으면 강제 회수(force 셈), 빈 칸 없으면 버림(full 셈);
  (2) MLME-DISCONNECTED.ind를 기록만 했음 → 횟수 세고 2초 뒤 재검색·재연결. heartbeat `XC5 w join cfm ind st disc <이유> n <횟수> | tx
  back <회수> full <버림> force <강제>`. 이미지 `xc5-ios-wifi22-console.tar`(드롭 wifi21과 같음 1794108066).
- **비슷한 사례 조사(2026-11-08)**: 가장 가까운 것은 **stormtroopxyz/y2sios**(MIT, Galaxy S20+, BCM4375 PCIe): README에 Wi-Fi "Working —
  WPA2, DHCP, IPv4/IPv6, HTTPS; signal, noise floor and security reported to iOS". iOS 쪽 층(칩 무관, 상류 hakios-sm7325에서 옴):
  `kernel/xnu/files/bsd/dev/redwood_net.c`(en0, ifnet_allocate_extended), `redwood_sta.c`(검색·연결 상태 기계, ops 표),
  `redwood_apple80211.c`(693줄: wifid/IO80211.framework의 Apple80211 ioctl 전부 흉내 — SCAN_REQ 10, SCAN_RESULT 11(1164 B 구조),
  ASSOCIATE 20(592 B), ASSOCIATE_RESULT 21, 이벤트 9 등, 16.5 IO80211.framework에서 레이아웃 확인), `iokit/Kernel/RedwoodWLAN.cpp`
  (165줄: IOService `IO80211Controller` + 그 아래 `IONetworkInterface`(속성 IO80211InterfaceRole="Infrastructure" 등)만 만들어 wifid가
  찾게 함), `userland/src/net_test.m`, 문서 `docs/plans/06-wireless.md`. WPA2 4-way는 Broadcom 펌웨어 쪽(ASSOCIATE의 RSN 키 정보는 안 씀).
  다른 사례: Liquidation0619/hakios-thyme-mi10s(QCA6390 ath11k, LinuxKPI, 검색까지), oosexgang/hakios-sm7325(WCN6750, y2sios의 상류).
  우리 쪽 적용: 설정 앱 Wi-Fi 목록·연결 = y2sios의 RedwoodWLAN + redwood_apple80211을 xc5_wlbt의 검색/연결 함수에 연결(MIT, 출처 표기).
  WPA2 = SCSC는 보통 호스트 wpa_supplicant가 4-way를 함; `MLME_SET_PMK_REQ 0x201c`(cfg80211 WLAN_CIPHER_SUITE_PMK, 키 관리 오프로드)가
  있어 펌웨어가 PMK로 4-way를 할 수도 있음(미확인) → 안 되면 커널에 PBKDF2·EAPOL·MLME-SETKEYS(0x2018) 구현.
- **4e 설정 앱 연동 구현(커밋 d4d4515 + f8d379f, xnu CI 42455532156)**: y2sios 파일을 `tools/xcover5/y2sios/`에(MIT 고지 머리말, LF): `redwood_sta.c`
  (station 스레드·마지막 검색 결과), `redwood_apple80211.c`(wifid의 SIOCGA80211/SIOCSA80211, 802.11 커널 이벤트 class 6), `redwood_sta.h`,
  `redwood_net.h`(우리용으로 축약), `RedwoodWLAN.cpp`(IO80211Controller + IONetworkInterface, IO80211InterfaceRole=Infrastructure; 블루투스 부분 뺌).
  apply-xcover5.py가 bsd/dev, bsd/sys, iokit/Kernel/arm에 복사·conf 등록. **부팅 인자 `xc5wifiui=1`**: VIF 생성 뒤 자동 검색·연결·커널 DHCP 안 함 →
  en0(subfamily WIFI, 미디어 802.11) 만들고 RedwoodWLANStart + redwood_sta_attach(xc5 ops: power/scan(우리 2.4 GHz 검색, 완료 대기)/heard(원소·
  capability·비컨 간격 포함)/join(개방형만: CONNECT 대기 → 링크 업 KEV_DL_LINK_ON → configd DHCP; WPA2는 EOPNOTSUPP)/leave(MLME-DISCONNECT 0x200e)/
  state/channels 1-13). 모르는 ioctl은 redwood_apple80211_ioctl로. 끊김 → redwood_sta_lost. heartbeat `XC5 w net ... | ui 1 phase <0 off,1 idle,
  2 scan,3 join,4 joined> link <0/1> scans joins leaves`. 이미지 `xc5-ios-wifi23-console.tar`(인자: wifi21에서 xc5net 빼고 `xc5wifiui=1`, 드롭 그대로).
  시험: 설정 > Wi-Fi 목록에 핫스팟이 보이는지, 눌러서 연결되는지(개방형).
- **사용자(wifi23): 설정에서 Wi-Fi가 안 켜짐.** y2sios README: "Working runs use wifi_pcie_wait=3 so en0 exists before wifid starts" → 우리는 en0·
  RedwoodWLAN을 펌웨어·서비스·VIF 뒤(수십 초)에 만들어 wifid가 장치를 못 찾음. 조치(커밋 e449248, xnu CI 42456383025): Wi-Fi 스레드 시작
  즉시(lo0가 생기면) en0·RedwoodWLAN·redwood_sta_attach, 그 전에 온 검색·연결 요청은 VIF가 생길 때까지 최대 90초 대기(sta_wait_ready).
  이미지 `xc5-ios-wifi24-console.tar`(인자 wifi23과 같음).
- **wifi24 결과(영상 20261108_160005)**: 여전히 스위치 꺼짐, `XC5 w net 0 ... | ui 1 phase 1 link 0 scans 0 joins 0` → wifid가 아무것도 묻지 않음.
  원인: 순정 `com.apple.wifid.plist`의 **`_LimitLoadToDeviceTree = ['wlan', 'marconi-wifi']`** — launchd가 장치 트리에 그 노드가 있을 때만 wifid를
  띄움. 우리 ADT(tools/mkadt_xcover5.py)는 arm-io 아래 wdt만 남겨 wlan이 없음 → **wifid가 아예 실행 안 됨**(y2sios는 mkjobs.py에서 이 키를
  지움). 조치(커밋 87fc9e1, xnu CI 42457341777): ADT에 `arm-io/wlan`(device_type wlan, compatible samsung,xcover5-wlbt, local-mac-address) 추가,
  y2sios **MiniIONetworkStack.cpp**(MIT, IOResources/IONetworkStack + 열 수 있는 user client: configd 인터페이스 namer가 이게 없으면 레지스트리
  quiet를 5분 뒤에야 선언 → wifid의 SCNetworkInterfaceCopyAll이 그만큼 대기) 추가, ui_up에서 시작. 이미지 `xc5-ios-wifi25-console.tar`.
- **wifi25 결과(영상 20261108_162240): wifid 실행됨 — 설정의 Wi-Fi 스위치 켜짐**, 네트워크 목록은 빙글(핫스팟 안 뜸). `XC5 w ... ui 1 phase 1 link 0
  scans 9 joins 0`, 콘솔 `apple80211: wifid gets 19 (POWER)`, `wifid gets 103 (CURRENT_NETWORK) ... error 6`(연결 전이라 정상). locationd 반복 크래시.
  진단(커밋 9396409, xnu CI 42458833319): heartbeat `XC5 w a80211 req <요청> refused <거절> (last <타입, set은 +1000> err <errno>) | scan aps <마지막
  검색에서 찾은 AP 수> done <검색 끝 횟수> ev <SCAN_DONE 이벤트> | results asked <SCAN_RESULT(11) 요청> given <AP를 준 횟수>`. 이미지 `xc5-ios-wifi26-console.tar`.
- **wifi26 결과(영상 20261108_163657)**: `XC5 w a80211 req 286 refused 168 (last 103 err 6) | scan aps 0 done 0 ev 1 | results asked 1 given 0` →
  나중에 `ev 2 asked 2 given 0`. 즉 wifid가 검색 요청 → 커널이 펌웨어에 ADD_SCAN을 보냄(scans 1) → **펌웨어 SCAN_DONE이 한 번도 안 옴, 결과 0** →
  12초 뒤 시간 초과로 SCAN_DONE 이벤트 → wifid가 결과를 읽지만 0개. 그 외 wifid가 `gets 115 (?) 1288 bytes: REFUSED`(미구현), `gets 103 error 6`(연결 전 정상).
  진단(커밋 8b8c013, xnu CI 42459941247): heartbeat `XC5 w fw vif <ADD_VIF 결과> scancfm <이번 ADD_SCAN 확인, 검색마다 초기화> ind <SCAN_IND 수> sent got ids
  <최근 신호 4개>`. 이미지 `xc5-ios-wifi27-console.tar`.
- **wifi27 결과(영상 20261108_165518)**: `XC5 w fw vif 0 scancfm ffff ind 0 sent 3 got 10 ids 8301 2101 8301 2107` → wifid 요청으로 **redwood_sta 스레드에서
  보낸 ADD_SCAN에 확인(CFM)조차 없음**, ADD_VIF 확인 뒤 펌웨어 무응답. 지금까지 응답받은 신호는 모두 Wi-Fi 스레드의 hip_poll 안에서 보낸 것.
  조치(커밋 de5e483, xnu CI 42461129954): station ops는 요청만 표시(want_scan/want_join/want_leave), hip_poll이 보냄; UI 모드에서도 ADD_VIF 직후
  첫 검색을 바로(예전 동작 방식), 결과는 wifid용으로 보관. 원인(다른 스레드에서의 q_put이 왜 무시되는지)은 미확인. 이미지 `xc5-ios-wifi28-console.tar`.
- **wifi28 결과(영상 20261108_171422)**: `XC5 w fw vif 0 scancfm ffff ind 2 sent 10 got 82`, `XC5 w cfm 2 vif 0 scan ffff ind 2 done 1 aps 0` → 첫 검색(ADD_VIF 직후)은
  AP 2개·SCAN_DONE 정상. 하지만 이후 wifid 검색마다 목록 비우고(ap_count=0) 같은 scan id로 ADD_SCAN 반복 → 확인 없음 → 목록 0. 조치(커밋 b1009d1,
  xnu CI 42462437237): 찾은 AP 보존(BSSID로 중복 제거), 20초 안에 끝난 검색이 있으면 wifid에 바로 응답, 진행 중이면 재요청 안 함, 다시 검색할 땐
  **MLME-DEL-SCAN.request 0x2006**(scan id 0x100) 뒤 ADD_SCAN. 이미지 `xc5-ios-wifi29-console.tar`.
- **wifi29 결과(영상 20261108_173146): 설정 Wi-Fi 목록에 핫스팟 S24가 뜸!** 누르면 "다음 네트워크에 연결할 수 없음: 'S24'". 콘솔 `XC5 w cfm 2 vif 0 scan ffff ind 1
  done 1 aps 1`, `XC5 w ap S24 2437 -30 o`, `XC5 w join cfm ffff ind ffff`(CONNECT 확인조차 없음), `ui 1 phase 1 ... scans 4 joins 2 leaves 2`, `a80211 req 403
  refused 327 ... results asked 8 given 4`, fw 로그 `deep_sleep_wake.c: CMN: [DEEPSLEEP] infrastructure`. → 펌웨어가 한동안 쉬면 딥슬립, 그 뒤 보낸 신호에 무응답.
  우리는 펌웨어→AP 인터럽트를 한 번도 지우지 않았음(삼성: platform_mif_irq_bit_clear, INTCR0 = bit<<16). 조치(커밋 66862e9, xnu CI 42463772327):
  hip_poll이 큐를 읽기 전에 INTSR0의 대기 비트를 INTCR0(0x00c)로 지움. heartbeat `XC5 w fw ... irqclr <횟수> sr0 <INTSR0>`. 이미지 `xc5-ios-wifi30-console.tar`.
  그래도 안 되면: 딥슬립 MIB 끄기(펌웨어 문자열 "DEEP SLEEP has been DISABLED ... via MIB override"), 또는 깨우기 절차 조사.
- **wifi30 결과(영상 20261108_175109)**: 목록에 **S24와 olleh_WiFi_FE1A 둘 다** 뜸(검색 보존·DEL_SCAN 효과), `irqclr 61 sr0 0`(인터럽트 지우기 동작). S24 누르면 여전히
  "연결할 수 없음", 그런데 `sent 7 got 81`이 연결 시도 전후로 같음 → **CONNECT 자체를 안 보냄**(펌웨어 문제 아님). 원인 위치 진단(커밋 3218814, xnu CI 42466106103;
  첫 실행은 CI의 hdiutil KDK 마운트 실패로 재실행): heartbeat `XC5 w ui join assoc <ASSOCIATE 수> why <1 짧은 요청,2 이름,3 키 종류,4 넘김> cipher <키 종류> key <길이>
  | stage <1 대기,2 보안,3 AP 못 찾음,4 CONNECT 요청,5 응답> err <errno> sec ssid bssid | cfm ind`. 이미지 `xc5-ios-wifi31-console.tar`.
- **wifi31 재촬영(영상 20261108_184020)**: `XC5 w ui join assoc 1 why 4 cipher 0 key 0 | stage 5 err 60 sec 0 ssid 3 bssid 0 | cfm ffff ind ffff`, `XC5 w fw ... ind 3 sent 6 got 80
  irqclr 12`, `scan aps 3 done 1`. → wifid의 연결 요청은 정상(개방형, 키 0) → 드라이버가 CONNECT 보냄(stage 4) → 15초 무응답(ETIMEDOUT). 그때 두 번째 검색(DEL_SCAN·
  ADD_SCAN 연달아)이 안 끝난 상태(ind 3 done 1) → 펌웨어가 검색 중 CONNECT를 보류하는 것으로 추정. 조치(커밋 93fd461, xnu CI 42468613276 — 1·2차는 CI의 KDK
  다운로드가 GitHub 504로 실패, dortania/KdkSupportPkg 릴리스 회복 뒤 3차 성공): 연결 전 항상 DEL_SCAN → 확인(0x2106) 또는 2초 뒤 CONNECT, 새 검색도 DEL_SCAN
  확인 뒤. heartbeat `XC5 w ui join assoc why key | stage err | cfm ind | del <DEL_SCAN 확인> next <대기 중 다음 동작>`. 이미지 `xc5-ios-wifi32-console.tar`.
- **wifi32 결과(영상 20261108_191510)**: 여전히 "연결할 수 없음"(콘솔 진단 줄은 영상에 안 잡힘). 성공했던 연결(wifi17-22)은 모두 검색이 끝난 순간 바로 보낸 CONNECT였고,
  삼성 드라이버도 연결 직전 그 네트워크를 다시 검색함(cfg80211_ops.c slsi_connect → slsi_mlme_connect_scan). 조치(커밋 1efcac0, xnu CI 42470910224): 연결 요청 →
  DEL_SCAN → 확인 뒤 새 ADD_SCAN → **SCAN_DONE을 받는 즉시 CONNECT**, 연결 대기 25초, 실패하면 콘솔에 바로 `XC5 WLBT: JOIN FAILED cfm ind del scans done sent got`,
  성공하면 `XC5 WLBT: joined (from Settings)`. 이미지 `xc5-ios-wifi33-console.tar`(실제 빌드는 xnu CI 42471158927 — 1efcac0의 두 실행이
  KDK 504로 실패해 워크플로에 KDK 캐시·재시도 추가, 커밋 fa4999c).
- **wifi33 결과(영상 20261108_194003)**: 여전히 실패. 50초 하트비트 `ui join assoc 1 why 4 key 0 | stage 4 err -1 | cfm ffff ind ffff |
  del ffff next 0`, `fw vif 0 scancfm ffff ind 3 sent 7 got 86 irqclr 24 sr0 0 ids 8301 8301 8301 2301` — 35초부터 55초까지 got 86 그대로,
  마지막 신호가 첫 SCAN_DONE(0x2301). 즉 **첫 검색이 끝난 뒤 펌웨어가 아무것도 보내지 않음**: 연결의 DEL_SCAN도 확인 없음(del ffff),
  뒤 ADD_SCAN도 끝나지 않아 CONNECT가 나가지 못함(wifi28·29와 같은 현상, fw 로그 `[DEEPSLEEP] infrastructure`). 삼성 소스 대조:
  도어벨 INTGR1·TH_RFB 반환·PMU ACTIVE_EN 모두 같음. 조치(커밋 19c8c28, xnu CI 42473180519): ui 모드에서 연결 전까지 **SCAN_DONE마다
  다음 검색을 바로 시작**(펌웨어를 쉬게 두지 않음), 검색 도중 연결 요청이 오면 DEL 없이 그 검색의 SCAN_DONE에서 CONNECT. 새 하트비트
  `XC5 w keep done <검색 완료 수> chain <이어 붙인 검색> onrun <검색 중 받은 연결> | q0 w r(FH 제어 큐) q3 w r(TH 제어 큐) | fw: <펌웨어 마지막 로그>`.
  done이 계속 늘면 가설(쉬면 잠듦) 맞음; done이 1에서 멈추면 펌웨어가 첫 검색 뒤 바로 멎는 것 → q0 r(펌웨어가 큐를 읽는지)·fw 로그로 판단.
  이미지 `xc5-ios-wifi34-console.tar`.
- **wifi34 결과(영상 20261108_205414)**: 여전히 실패. `keep done 22 chain 22 onrun 3 | q0 w47 r46 q3 w82 r82 | fw: Speedy drive strength
  changed from 1 to 2`가 44초~59초 그대로, `ui join assoc 3 ... stage 4 err 60 | cfm ffff ind ffff | del 0`. 즉 이어 붙인 검색은 22번 정상
  (펌웨어가 쉬지 않으면 응답함 — 가설 일부 맞음), 그러다 **펌웨어가 FH 제어 큐를 더 읽지 않음**(w47인데 r46), 그 뒤로 아무 신호도 없음.
  세 연결 모두 실행 중인 검색 위에 걸렸는데 그 검색의 SCAN_DONE이 끝내 안 와서 CONNECT는 한 번도 나가지 못함. 자는 것인지(PMU STATES),
  멎은/패닉인지, 도어벨(INTSR1 bit5)이 걸린 채인지 아직 모름.
  사례 조사: SCSC/WLBT를 공개 저장소에서 다룬 사례는 거의 없음(postmarketOS Galaxy A13 = Exynos 850, Wi-Fi 미동작; exynos850-mainline
  목록에 Wi-Fi 없음). 삼성 소스(A21s mlme.c/cfg80211_ops.c/mgt.c) 대조: CONNECT 내용은 우리와 같음. 차이: 삼성은 **연결할 때 ADD_VIF**
  (slsi_connect: add_vif → register_action_frame → set_boost → set_ext_cap → CONNECT, 검색은 망 정보가 없을 때만), 시작 때
  MLME_SET_COUNTRY와 host_state(CELLULAR_ACTIVE) 설정. 조치(커밋 772e3f8): 큐에 300 ms 넘게 남아 있으면 도어벨을 1초마다 다시 울림
  (`kick n/ok`), 콘솔 `XC5 WLBT: FW STALL q0 w r ms | st sr1 msr1 | got done`, 하트비트 `XC5 w why st sr1 msr1 issr0 logw recs kick stall pan`.
  이미지 `xc5-ios-wifi35-console.tar`. 다음 후보: 연결 때 DEL_VIF/ADD_VIF 후 바로 CONNECT(삼성 순서), MLME_SET_COUNTRY.
- **wifi35 결과(PREV BOOT LOG 사진 18장)**: `XC5 w why st 10 sr1 20 msr1 20 issr0 2222001b logw 203 recs 58 kick 161/0 stall 161961 ms
  pan 0 0`, `keep done 4 chain 4 onrun 1 | q0 w11 r10`, `got 214`, ids 마지막 `2301 2106`, 요약 `STATUS 0x1 STATES 0x10 ... INTSR1 0x20 |
  mgmt r2 w2, mxlog w203 | panic 0`. 즉 전원은 켜진 상태(STATES 0x10은 정상 때와 같음), **우리 도어벨(INTSR1 bit5)이 걸린 채 펌웨어가
  인터럽트를 아예 처리하지 않음**, 다시 울려도(161번) 소용없음, 패닉 기록·mgmt 메시지 없음, 로그 멈춤(마지막 `Speedy drive strength
  changed from 1 to 2` — wifi34도 같은 마지막 줄). 펌웨어 R4가 멎음. **시점**: 같은 로그에서 RTC 줄 `calendar now ... (uptime 190 s)`
  바로 뒤 하트비트가 stall 162 s → 멎은 때 ≈ 부팅 후 33초, 커널 RTC 코드(xc5_pmic.c rtc_sync)가 ACPM으로 PMIC를 읽고 **쓰는** 때가
  uptime 30초. WLBT도 PMIC(SPEEDY 버스)를 씀(마지막 로그가 Speedy) → 충돌 의심. 시험 이미지 `xc5-ios-wifi35r-console.tar`
  (= wifi35 + `xc5rtc=0`, ACPM 접근 없음; 날짜는 1970). 멈춤이 사라지면 RTC를 Wi-Fi 전에(또는 쓰기 없이) 하도록 고침.
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
