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
## 지금 상태 (기록은 2026-11-07부터 시간순, 최신은 이 파일 맨 끝)

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
- **wifi35r 결과(사진 20장)**: RTC를 꺼도 똑같이 멈춤 → **RTC 원인 아님**. `keep done 7 chain 7 | q0 w17 r16`, `why st 10 sr1 20 kick 159/0
  stall 159716 ms`, 마지막 fw 로그 같음(Speedy drive strength 1→2). heartbeat 39 `t=5117057448`(26 MHz → 196.8 s) 근처에서 stall 160 s →
  멈춘 때 ≈ 부팅 후 35~37초(wifi35도 비슷). 삼성 platform_mif_3830.c: "The APM IPC in FW will be used instead" — 펌웨어는 PMIC(LDO 등)를
  APM 메일박스(APM2WLBT, 0x119B0000)로 요청. 그 응답이 없어 멈추는지 확인용 진단 추가(커밋 649ff91): 하트비트 `XC5 w apm sr0 msr0 sr1 msr1
  issr0-3 | stall at <uptime s>`, 멈춘 지 5초 뒤 콘솔 `XC5 WLBT: FW LOG -4..-0`(펌웨어 마지막 로그 5줄). 이미지 `xc5-ios-wifi36-console.tar`.
- **wifi36 결과(영상 20261108_214952)**: 연결 실패 같음. `keep done 23 chain 23 onrun 1 | q0 w49 r48 ... | fw: Speedy drive strength changed
  from 1 to 2`, `why st 10 sr1 20 msr1 20 issr0 2222001b ... kick 111/0`, **`apm sr0 0 msr0 0 sr1 0 msr1 0 issr d50179 30002 d50000 0 | stall
  at 50 s`** → APM2WLBT 메일박스에 걸린 인터럽트 없음(APM 응답 대기로 멈춘 것은 아닌 듯). 멈춘 시점은 매번 다름(wifi33 검색 1번, 34 22번,
  35 4번, 35r 7번, 36 23번; 부팅 후 30~50초대) — 고정 시각이 아니라 검색 도중 어느 때. 마지막 fw 로그는 항상 Speedy 줄.
  `FW LOG -4..-0`(한 번만 찍힘)은 영상에 안 잡힘 → PREV BOOT LOG에서 읽어야 함.
- **wifi36 두 번째 부팅(PREV BOOT LOG 사진 20장)**: `keep done 51 chain 51 onrun 1 | q0 w105 r103`, `stall at 66 s`, `kick 129/0`, `apm` 모두 0,
  `got 2156`. 펌웨어 로그는 이번에도 `log w203 recs 58`(wifi34·35·35r·36 모두 같은 값) → 펌웨어 로그는 멈춤과 상관없이 부팅 초기 고정
  지점에서 끊김(멈춤 시점은 매번 다름) → FW LOG 줄은 단서가 안 됨. 정리: 펌웨어가 검색 1~51번 뒤 무작위로 완전히 멎음(전원 켜짐, 도어벨
  미처리, 패닉 없음). **자동 모드(xc5net=1, wifi19-22)에서는 몇 분간 연결·ping·DNS가 됐음** → ui 모드에만 있는 것(wifid/RedwoodWLAN/
  IOKit 스택, 연속 검색)이 원인인지, 지금 커널 전체 문제인지 가르기 위해 비교 이미지 `xc5-ios-wifi36a-console.tar`(같은 커널,
  `xc5net=1`, wifiui 없음) 시험. 자동 모드가 2분 넘게 살아 있으면(ping 증가) ui 모드 쪽을 좁힘.
- **wifi36a 결과(사진 20장)**: 자동 모드는 **정상** — `done 1`, `join cfm 0 ind 0`, `ip 10.168.19.211`, `dns 10.168.19.73`, ping 58→68/68,
  data tx 8846 / rx 21468, `got 32304`, 펌웨어 멈춤 없음(heartbeat 32~36 내내 증가). → 커널·펌웨어·전원은 문제없고 **ui 모드에만 있는 것**이
  원인. 차이: 자동 모드는 검색 1번 후 바로 CONNECT, **MLME-DEL-SCAN을 한 번도 안 보냄**; ui 모드는 wifi28부터 끝난 검색마다 DEL_SCAN 후
  ADD_SCAN(34부터는 연속). 삼성 드라이버는 DEL_SCAN을 진행 중인 검색 중단에만 씀(끝난 검색엔 안 씀). 조치(커밋 5f93d21): 검색·연결용 검색은
  ADD_SCAN만, 연결은 그 SCAN_DONE에서 CONNECT, DEL_SCAN은 15초 넘게 SCAN_DONE이 없는 검색에만, 연속 검색 끔(`xc5wchain=1`로 켬). 하트비트
  `XC5 w keep done chain abort onrun | q0 ...`. 이미지 `xc5-ios-wifi37-console.tar`. (참고: locationd가 계속 SIGTRAP으로 죽음 — 별건.)
- **wifi37 결과(사진 20장)**: 여전히 실패, 더 빨리 멎음: `keep done 1 chain 0 abort 25 | q0 w53 r3`, `got 87`, `why sr1 20 msr1 20 kick 64/0`.
  펌웨어는 처음 3개 신호(GET, ADD_VIF, ADD_SCAN)만 읽고 첫 검색 결과·SCAN_DONE을 보낸 뒤 **그 다음 신호부터 아예 안 읽음**. → DEL_SCAN 탓이
  아니라 **쉬는 틈**이 원인: ui 모드는 첫 검색 뒤 wifid가 다음 요청을 보낼 때까지 아무것도 안 보냄(→ 펌웨어 딥슬립, 도어벨로 안 깨어남;
  wifi29 로그 `[DEEPSLEEP] infrastructure`), 자동 모드는 SCAN_DONE 즉시 CONNECT → DHCP·ARP·ping이 이어져 쉴 틈이 없음. wifi34~36의 연속
  검색이 1~51번 버틴 것도 같은 이유(검색 사이 DEL_SCAN 확인 대기 2초가 가끔 틈이 됨). 조치(커밋 cd247a7): 큐가 비고 `xc5wka`ms(기본 200,
  0=끔) 동안 보낸 것이 없으면 조용한 MLME-GET(unifiChipVersion) 1개 전송 — 펌웨어를 쉬지 않게. 하트비트 `keep ... ka <보냄>/<응답>`.
  이미지 `xc5-ios-wifi38-console.tar`. ka 응답이 계속 늘고 q0 r가 w를 따라가면 해결 방향 맞음. 근본 해결(깨우는 방법) 후보: PMU
  WLBT_INT_EN(0x3144) [3],[5] — 삼성 리셋 해제 주석에는 있으나 코드엔 없음.
- **wifi38 결과(사진 20장)**: 실패. `ka 11/10 | q0 w30..44 r13`, `got 98`, 마지막 ids `2101 2101 2101 2101`, `stall at 33~37 s`, `JOIN FAILED
  cfm ffff ind ffff del ffff scans done 1 sent 44 got 98`. → keepalive GET은 10번 응답받다가 첫 SCAN_DONE 약 2초 뒤 펌웨어가 멎음 —
  GET으로는 막지 못함. 펌웨어 MIB 설명: **UnifiIdleModeEnabled(0x09DF) "단일 station vif이거나 vif 없음 + 한동안 활동 없음이면 Idle Mode"**,
  UnifiStaIdleModeEnabled(0x09BD) 같은 조건 → 연결 안 된 ui 모드가 정확히 이 경우(자동 모드는 바로 연결해 해당 없음). 조치(커밋 bfdc7a4):
  GET 응답 뒤 ADD_VIF 전에 **MLME-SET 0x2002로 0x09DF·0x09BD·0x09DE = 0**, SET_CFM(0x2102) 뒤 ADD_VIF. `xc5widle=1`이면 건너뜀. GET이 세 값의
  hcf 값도 읽어 하트비트 `XC5 w keep idle <09DF> <09BD> <09DE> set <결과> | done ... ka ...`. 이미지 `xc5-ios-wifi39-console.tar`.
  set 0이고 ka가 계속 늘면 해결. set이 0이 아니면(거부) 다른 방법(키 이름·형식) 확인.
- **wifi39 결과(사진 20장)**: `keep idle 0 1 1 set 0`(hcf 값: 0x09DF=0, 0x09BD=1, 0x09DE=1 → SET 수락), `ka 66/65 | q0 w100..110 r69`,
  `got 176`, `stall at 45 s`, ids `8302 2101 8302 2101`. → 유휴 모드를 끄니 약 13초 더 버텼지만 결국 멈춤. 멈추기 전 펌웨어가
  **DEBUG_FAULT_IND(0x8302)** 를 보냄 = 펌웨어 자체 오류 보고 {vif; faultid; count; timestamp; arg; cpu}(fapi.h). 지금까지는 버리고
  있었음. 조치(커밋 6aeb4dd): 콘솔 `XC5 WLBT: FW FAULT id count arg cpu (n got)`(처음 12개), 하트비트 `XC5 w fault n id count arg cpu`.
  이미지 `xc5-ios-wifi40-console.tar`. faultid로 원인 좁히기(공개 자료에 목록은 없음 — id·arg 패턴과 멈춤 시점 대조).
- **wifi40 결과(사진 20장)**: 이번엔 오류 보고 없음(`fault n 0`), `keep idle 0 1 1 set 0 | done 1 abort 24 | ka 4/3 | q0 w56 r7`,
  `got 88`, `stall at 32 s`, `kick 149/0`. → 첫 SCAN_DONE 약 1초 뒤 멈춤(wifi38 2 s, 39 13 s, 40 1 s — 무작위). 유휴 모드 끄기는 결정적이지
  않음. y2sios 코드는 하드웨어를 건드리지 않음(확인). 남은 차이 후보: 삼성 리셋 해제 순서(PMUCAL v2 주석)에 있는 **MIF_CTRL 0x3810[0],
  TCXO_BUF_CTRL 0x3b78[0], WLBT_INT_EN 0x3144[3][5]** 를 우리는 설정한 적 없음 — 쉬는 코어가 메모리·26 MHz 클록·깨우기 인터럽트를 잃을 수
  있음. 조치(커밋 2e91156): 전원 켤 때 세 레지스터 설정(`xc5wpmu=0`이면 안 함), 콘솔 `XC5 WLBT: MIF_CTRL a -> b, TCXO_BUF_CTRL ..,
  WLBT_INT_EN ..`, 하트비트 `XC5 w fault ... | pmu mif x>y tcxo x>y inten x>y`. 이미지 `xc5-ios-wifi41-console.tar`.
- **wifi41 결과(사진 20장)**: `pmu mif 3>3 tcxo 1>1 inten 3f>3f` — 세 레지스터는 **이미 켜져 있었음**(S-Boot/펌웨어가 설정) → PMU 가설 탈락.
  `ka 34/33 | q0 w70..86 r37`, `stall at 38 s`, 오류 보고 없음. 관찰: 매 부팅 **r = 처음 신호 수 + ka 응답 수** (wifi38 3+10=13, 40 4+3=7,
  41 4+33=37) — 펌웨어는 받은 keepalive를 모두 처리하다가 **다음 keepalive를 꺼내기 전에** 멎음. 즉 특정 신호가 원인이 아니라 신호 사이
  0.2초 빈틈에서 멎음(딥슬립 진입 후 못 깨어나는 것과 맞음). MIB에 칩 딥슬립 끄기 키는 없음(mib.h: 0x09BE/C4/C5/CE/E4/E8 등은 연결 중 절전).
  빌드 없이 시험: `xc5-ios-wifi41k-console.tar` = wifi41 + **`xc5wka=5`**(keepalive 5 ms 간격). ka가 계속 늘면(수천) 빈틈 가설 확정 →
  다음: 짧은 keepalive 유지 + 연결 시도, 근본책은 펌웨어 슬립 허용/금지 신호(mxman·`scsc_mx` 쪽 "host sleep"/WLBT_ACTIVE 처리) 찾기.
- **wifi41k 결과(사진 20장)**: `ka 34/33 | q0 w74..88 r37`, `stall at 34 s` — **200 ms(wifi41)와 5 ms에서 똑같이 keepalive 33번 뒤 멈춤**
  → 시간(빈틈)이 아니라 **개수** 문제. 빈틈 가설 탈락. 원인 발견: 펌웨어→호스트 신호 중 데이터가 붙은 것(GET 확인, 검색 결과)은 mbulk
  **체인**(머리 + chain_next)으로 옴. 삼성 hip4.c `hip4_mbulk_to_skb`는 머리와 **체인의 모든 버퍼**를 TH_RFB(q5)로 돌려줌. 우리는 머리만
  돌려줌 → 펌웨어 송신 버퍼 풀이 고갈 → 응답을 못 만들어 요청 처리 중단. 자동 모드는 이런 신호가 적어 안 걸렸음.
  (붙여넣은 외부 분석의 "버퍼 반환 누락" 지적은 방향(FH↔TH)은 달랐지만 같은 종류의 문제.) 조치(커밋 15eb609): 체인의 모든 버퍼 반환,
  하트비트 `XC5 w rfb <반환> chained <추가 버퍼> full <q5 가득>`. 이미지 `xc5-ios-wifi42-console.tar`(keepalive 기본 200 ms).
- **wifi42 결과(사진 20장)**: `rfb 329 chained 163 full 0`(체인 반환은 동작 — 받은 신호 166개 거의 모두 2버퍼 체인) 그런데도 `ka 10/9 | q0 w68
  r13`, `stall at 33 s`, `got 166` → **체인 반환 가설도 탈락**. 멈춤 시각 정리: wifi35r 35 s, 38 33~37, 39 45, 40 32, 41 38, 41k 34, 42 33 —
  거의 늘 **부팅 후 약 33초**(wifid·configd가 en0을 다루기 시작하는 때). 자동 모드(wifi36a)는 같은 커널로 몇 분 정상. 남은 갈래:
  (가) 연결 안 된 펌웨어 자체가 ~30초에 멎음, (나) ui 모드의 iOS 쪽(wifid/configd/RedwoodWLAN) 활동이 원인. 빌드 없이 가르는 이미지:
  `xc5-ios-wifi42n-console.tar` = 같은 커널, **자동 모드(`xc5net=1`)인데 `xc5ssid=XC5NONE`**(없는 이름이라 연결 안 하고 8초마다 재검색,
  keepalive도 동작, wifid는 커널 Wi-Fi와 연결 안 됨). 2분 넘게 `done`·`ka`가 계속 늘면 (나), 33초쯤 멈추면 (가).
- **wifi42n 결과(사진 20장)**: 자동 모드·미연결(`ui 0 phase 1`)에서도 `done 1`, `got 160`이 heartbeat 28~32(약 140~160 s) 내내 그대로, 요약
  `INTSR1 0x20` → **iOS 쪽(wifid/configd) 원인 아님**, 연결 안 된 채 신호를 많이 주고받으면 멎음. 단서 둘: (a) 멎을 때까지 펌웨어가 읽은
  신호 수가 wifi41·41k 37(=32+5), wifi39 69(=64+5) — 제어 신호 슬롯 32칸을 반환 확인 없이 돌려 씀(데이터 슬롯은 추적함, 연결된 실행은 제어
  신호 32개 미만). 단 13·7·3에서 멎은 실행도 있어 확정 아님. (b) 멎는 시각이 대부분 ~33초 = Wi-Fi 스레드의 100 ms 루프(20초)가 10 ms 루프로
  바뀌는 때와 비슷. 조치(커밋 951ded7): 제어 슬롯을 FH_RFB로 돌아온 뒤에만 재사용(`ctl back/force/busy`), 두 루프 시작 시각 기록, `xc5wpoll=<ms>`
  로 둘째 루프 주기 지정. 하트비트(두 모드 공통) `XC5 w ctl back force busy | loop1 s loop2 s poll ms | q0 w r ka stall at kick`.
  이미지 `xc5-ios-wifi43-console.tar`(ui 모드), `xc5-ios-wifi43p-console.tar`(+`xc5wpoll=100`).
- **wifi43 결과(사진)**: `ctl back 23 force 11→25 busy ffffffff | loop1 30 s loop2 51 s poll 10 ms | q0 w66 r23 ka 20/19 stall at 35 s`.
  멈춤(35 s)은 첫 루프 안(둘째 루프 51 s 시작 전)이고, 읽은 23개 슬롯은 모두 반환됨(force는 멈춘 뒤 쌓인 것) → **폴링 주기·슬롯 재사용 둘 다
  원인 아님**(슬롯 추적은 그대로 둠). 전체 기록 재정리: 펌웨어는 **무선이 일하는 동안만** 살아 있음 — 연결됨(몇 분), 연속 검색(22~51번,
  30~66 s; DEL_SCAN 확인 2초 대기가 틈), 그 외에는 마지막 검색 뒤 1~13초에 멎음(keepalive GET 유무 무관). 조치(커밋 6576ffa): 연결 전에는
  SCAN_DONE 즉시 DEL 없이 다음 ADD_SCAN(`xc5wchain=2` 기본; 1=DEL 먼저, 0=끔), 연결 요청은 진행 중 검색의 SCAN_DONE에서 CONNECT.
  이미지 `xc5-ios-wifi44-console.tar`. 볼 것: `keep done`·`chain`이 계속 늘고 `stall at 0`인지, 그 뒤 S24 연결.
  같은 scan id 재-ADD가 거부되면(`scancfm` ≠ 0, done 정지) id를 번갈아 쓰기. 근본 원인(무선이 쉬면 왜 멎는가 — 딥슬립 복귀 실패 추정)은 미해결.
- **wifi44 결과(사진)**: 연속 검색도 실패 — `done 3 chain 3`, `q0 w159 r15`, `ka 9/9`, `stall at 33 s`, `got 467`, 검색은 3번(AP 3개) 뒤 멈춤.
  (`abort` 72와 반복된 0x2005/0x2006은 멈춘 뒤 15초 중단 경로가 보낸 것.) "무선이 쉬면 멈춤" 가설도 탈락. 정리: 펌웨어 시작(loop1 = 30 s)
  후 3~20초 안에 거의 늘 멈춤, 자동 모드에서 바로 연결한 경우만 몇 분 생존. 조치(커밋 29f8025): WLBT의 **WDOG·ALIVE 인터럽트**가 걸리는지와
  처음 걸린 시각, PMU STATUS/STATES/CTRL_NS/CTRL_S/DEBUG를 하트비트 `XC5 w wdog N at s alive N at s | pmu ...`로. 콘솔 `XC5 WLBT: WDOG interrupt
  pending at N s`. 이미지 `xc5-ios-wifi45-console.tar`. WDOG가 걸리면 펌웨어 워치독(호스트가 응답해야 하는 무엇) 쪽.
- **wifi45 결과(사진 20장)**: `wdog 0 at 0 s alive 0 at 0 s | pmu STATUS 1 STATES 10 CTRL_NS 21 CTRL_S 8 DEBUG 0` — **워치독·ALIVE 없음**,
  PMU 정상. 이번엔 더 일찍: `got 12`, ids `8301 2102 8301 2107`(GET·SET·ADD_VIF 확인까지), `q0 w93 r3`, `done 0`, `stall at 31 s`(펌웨어 시작
  `loop1 30 s` 약 1초 뒤, 첫 ADD_SCAN도 못 읽음). → 트래픽·검색·유휴와 무관하게 **시작 후 1~20초 사이 무작위로 R4가 큐 처리를 멈춤**(패닉·워치독·
  fault 없음, 로그는 늘 w203에서 이미 멈춰 있음). 살아남은 경우는 자동 모드 즉시 연결(wifi17-22, 36a)뿐. 다음 후보: (1) 더 새 Exynos 850
  벤더 커널(`MRIlla/android_kernel_samsung_a145f`, A14 4G Android 15)의 platform_mif_3830·hip4·mxman 초기화와 우리 순서를 줄 단위 대조,
  (2) Exynos 850 DT의 WLBT용 PMIC LDO always-on 여부(sm-t510 사례), (3) 자동 모드에서 성공한 실행과 같은 커널로 즉시 연결 재현(같은 커널로도
  사는지 = 커널 회귀 여부).
- **A14 소스 대조(2026-11-15)**: `MRIlla/android_kernel_samsung_a145f`(Galaxy A14 4G, Exynos 850, Android 15)의 platform_mif_3830.c·
  mxman.c·hip4.c를 A21s와 diff — 차이는 커널 5.4 API·PM QoS·procfs·NAPI·DUAL_FEM(UART_IO_SHARE_CTRL, 옵션)뿐, CFG_REQ·BAAW·PMU 순서·
  mxconf(fwconfig 0 포함)·HIP4 큐 처리는 같고 우리 구현과도 일치. 시작 때 삼성이 보내고 우리가 안 보낸 신호: **MLME-SET-COUNTRY 0x2033**
  (`slsi_start` → `slsi_mlme_set_country`, vif 0 전에). 조치(커밋 6e5aa3f): 유휴 모드 SET 확인 뒤 SET_COUNTRY "KR"(dfs 0, 규칙 없음) →
  확인(0x2133) 뒤 ADD_VIF. `xc5wcountry=0`이면 생략. 하트비트 `XC5 w country <결과> | wdog ...`. 이미지 `xc5-ios-wifi46-console.tar`.
- **wifi46 결과(사진 20장)**: `country 0`(SET_COUNTRY 수락), `done 28 chain 28`, `ka 56/56`, `got 4251`, AP 3개(S24 2412 -12 open 포함) —
  지금까지 가장 오래 동작(연속 검색 28번)했지만 `q0 w224 r88`, **`stall at 45 s`**로 결국 멈춤. 연결 요청(`assoc 1`)은 멈춘 뒤 와서
  `stage 5 err 60 cfm ffff`. wdog/alive/fault 모두 0. → 국가 코드는 도움은 됐을 수 있으나 해결 아님. 남은 유일한 생존 조건은 "펌웨어 시작
  직후 바로 연결"(자동 모드). 다음 후보: ui 모드에서도 첫 SCAN_DONE 때 커널이 열린 망(또는 지정 SSID)에 바로 연결하고 그 상태를 wifid에 넘김.
- **선연결(shadow join) 구현(커밋 f6c0ebb)**: 미연결 상태에서는 무엇을 해도 멈추고(wifi37-46, 버퍼 반환은 전부 정상: wifi46 rfb 8388 chained 4137
  full 0, q3 w155 r155) 즉시 연결한 자동 모드만 살아남으므로, 설정 앱 모드에서도 첫 SCAN_DONE에서 커널이 개방형 망(`xc5ssid=` 또는 가장 강한
  개방형)에 바로 CONNECT. link는 iOS에 내린 채 유지, 검색 요청은 캐시로 답함, 설정 앱이 그 망을 고르면 sta_join이 즉시 성공·link up
  (`XC5 WLBT: joined (from Settings, already joined by the kernel)`), 다른 망이면 먼저 DISCONNECT 후 기존 경로, leave는 연결 유지(link만 down),
  끊기면 다시 검색·재연결. 콘솔 `XC5 WLBT: joined S24 on its own (shadow)`, 하트비트 `XC5 w shadow tries ok used | ap ind`. `xc5wshadow=0`으로 끔.
  이미지 `xc5-ios-wifi47-console.tar`.
- **wifi47 결과(사진 20장 + 사용자)**: 선연결도 실패 — `shadow tries 1 ok 0 used 0 | ap 1 ind ffff`, `join cfm ffff`, `q0 w128 r7`, `stall at 32 s`
  (펌웨어 시작 2초 뒤), `done 1`. 첫 SCAN_DONE에 CONNECT를 보냈지만 확인조차 없음. 사용자: 첫 부팅은 설정에서 "연결 중"이 계속·"보안되지 않은
  네트워크" 표시, 두 번째 부팅은 S24가 "나의 네트워크"에 있으나 연결 안 됨. → 자동 모드(wifi36a)에서는 같은 순서로 몇 분 살았으므로
  **36a 이후 추가한 것 중 하나가 원인(회귀)**일 가능성. 빌드 없이 가르는 이미지 2개(같은 커널): `xc5-ios-wifi47a-console.tar` = 자동 모드
  (`xc5net=1`, 36a와 같은 인자; 여기서도 멈추면 커널 회귀 확정), `xc5-ios-wifi47m-console.tar` = 설정 앱 모드 + 36a 이후 추가분 끔(`xc5wka=0
  xc5widle=1 xc5wcountry=0 xc5wpmu=0 xc5wchain=0`, 선연결은 켬). 끌 수 없는 추가분: 제어 슬롯 추적·체인 버퍼 반환·15초 검색 중단·WDOG/APM 감시.
- **wifi47 재시험(사진 19장): 첫 성공 단계** — `shadow tries 1 ok 1 used 1 | ap 1 ind 0`, `ui join ... stage 6 err 0 | cfm 0 ind 0`,
  `phase 4 link 1`, 펌웨어 끝까지 살아 있음(`q0 w77 r77 ka 768/768 kick 0`, got 19000). 즉 선연결이 동작하고 설정 앱이 그 연결을 받음
  (부팅마다 성공/실패가 갈림 — 실패 부팅은 시작 2초 만에 멎어 CONNECT 무응답). 남은 문제: `data tx 12 cfm ffff rx 0`, `tx back 12`, `en0 addr -1`
  → iOS가 보낸 12개는 펌웨어가 모두 소비했지만 수신 0. 가설: iOS의 **비공개 Wi-Fi 주소**(네트워크별 임의 MAC)로 보내서 AP 응답이 vif가 받지
  않는 주소로 감. 조치(커밋 feb67f0): 송신 출발지·DHCP chaddr를 vif 주소로, 수신 vif 주소 앞 프레임을 iOS 주소로 되돌림(`xc5wmacfix=0` 끔).
  하트비트 `XC5 w txmac ios <iOS MAC> foreign <바꾼 수> mapped <되돌린 수> | types <첫 4개 ethertype> n | cfm rx`. 이미지 `xc5-ios-wifi48-console.tar`.
- **wifi48 결과(사진 20장)**: 선연결 다시 성공(`shadow ok 1 used 1`, stage 6, link 1, 펌웨어 끝까지 생존 `q0 w163 r163 ka 866/866`).
  `txmac ios 00:.. foreign 0 | types 86dd x4 n 12` → iOS는 비공개 주소를 쓰지 않았고(MAC 가설 탈락), **IPv6만 12개 보내고 DHCP(IPv4)는 아예
  안 보냄**; 수신 0, `cfm ffff`. fault 2개 `id 203c count 1 arg 3`. 조치(커밋 22c4c7a): 선연결 뒤에는 **커널이 직접 DHCP**(자동 모드에서 동작한
  그 코드) → en0에 주소·경로 설정 → kern.xc5wifi로 configd(libxc5net)에 전달. `xc5wdhcp=0`이면 끔. 이미지 `xc5-ios-wifi49-console.tar`.
  DHCP offer가 오면(`dhcp 2/3 offer ...`) 수신 경로는 정상, 안 오면 연결 후 수신 자체 문제.
- **wifi49 결과(사진 20장): 설정 앱 모드에서 처음으로 IP·인터넷 경로 성공** — `shadow ok 1 used 1`, stage 6, link 1,
  `data tx 88 rx 64 dhcp 3 offer/ip 172.24.72.211`, `if en0 addr 0 route 0 out 86 in 61 ping 56/56`, `dns 172.24.72.49`, 펌웨어 끝까지 생존
  (`q0 w163 r163 ka 605/605`, fault 0). types 800/806(IPv4·ARP) — 커널 DHCP가 동작. 남은 확인: 설정 앱 체크 표시·Safari 실제 동작(사용자 확인 필요),
  부팅마다 선연결 실패 확률(실패 부팅은 펌웨어가 시작 2초 만에 멈춤 — 근본 원인 미해결), locationd SIGTRAP(별건).
- **wifi49 화면(사용자)**: 설정 앱 체크 표시 안 뜸, Safari(apple.com)는 검은/하얀 화면. 원인 후보: configd 주입(libxc5net, IP·DNS를
  SCDynamicStore에 등록)은 `xc5net=1` 부팅 인자로만 켜짐(xc5_drop.c) — 설정 앱 모드 이미지엔 없어 `XC5 w net 0`(xc5netstat 0) → iOS가 주소·
  DNS를 모름. 커널은 xc5net 인자를 읽지 않으므로 빌드 없이 `xc5-ios-wifi49n-console.tar` = wifi49 + `xc5net=1`. 볼 것: `XC5 w net <0이 아닌 값>`
  (bit0 기록, bit1 우리 인터페이스가 기본, 0x10 HTTP 응답). Safari 화면 문제는 예전 w30r(EAGL) 수정이 실기기 미확인인 별건일 수 있음.
- **wifi49n/50 결과: 설정 앱 체크 표시 성공(2026-11-15)** — 49n: `net 0x1f`(configd 등록·기본 인터페이스·DNS·HTTP 응답), ping 76/76.
  체크 표시는 안 뜸 → 원인: en0에 BPF 없음(IPConfiguration의 DHCP는 BPF 사용; iOS는 IPv6만 보냈었음). 조치(커밋 0507819) bpf_attach +
  bpf_tap_in/out → **wifi50에서 설정 앱 체크 표시 뜸**(사용자 확인). Safari 검은/하얀 화면은 네트워크 아닌 화면 그리기 문제(별건).
  wifid 거절 요청: 115 = POWER_DEBUG_INFO(무관), 383 = 미확인.
- **WPA2-PSK 구현(커밋 076a67f)**: 커널이 4-way 핸드셰이크 수행 — PMK(wifid가 준 PMK 또는 비밀번호→PBKDF2-SHA1 4096), PTK(PRF-SHA1),
  EAPOL-Key M2/M4(HMAC-SHA1 MIC), M3 key data AES key unwrap(RFC 3394)로 GTK, 그룹 재키(G1→G2), M4 100 ms 뒤 MLME-SETKEYS 0x2018
  (pairwise CCMP 0x000FAC04 주소=AP, group은 AP RSN의 그룹 암호 CCMP/TKIP, 주소 FF.., RSC). CONNECT 데이터에 SSID 뒤 우리 RSN IE.
  10초 안에 끝나지 않으면 EAUTH(비밀번호 틀림)·DISCONNECT. 하트비트 `XC5 w bpf .. | wpa on done m1 m3 g1 micbad unwrapbad keys res gc gtk`,
  콘솔 `XC5 WLBT: WPA2 FAILED ...` / `joined (from Settings) with WPA2`. 이미지 `xc5-ios-wifi51-console.tar`(설정 앱 모드 + xc5net=1).
  미확인: SETKEYS 형식(FAPI 레이아웃 오프셋 vif10 len12 id14 type16 addr18 seq24 cipher40), M4 전 키 설치 순서, 펌웨어가 EAPOL을 데이터
  경로(MA_UNITDATA_IND)로 올리는지.
- **RAM 확대 시험(2026-11-16, 커널 그대로, `XC5_DRAM` 환경변수로 mkimage)**: 셋 다 패닉. 1408 MiB(`0x8a2800000:0x58000000`):
  `kmem_range_init: claim 10/17 gIOKitPageableFixed1 kr 3 size 0x6000000`, 앞선 **compressor claim이 0x58023c000(약 22 GB)** 로 커널 VA를
  다 먹음 → 압축기 크기 계산이 RAM에 비례해 폭증(4K 페이지 커널 문제 추정). 1280/1152 MiB: AMFI 등록 직후 `write_pte_fast: PTE write is
  unaligned, ptep=..af8, pte=0x4200008a5e48ec3 @pmap.c:623` → 4K 페이지 A5XELTE pmap이 1 GiB를 넘는 물리 영역(0x8a5e48000 등)의 PTE
  쓰기에서 정렬 검사 실패. 둘 다 커널 수정 필요(압축기 VA 상한, pmap PTE 정렬/PAGE_RATIO). 기본값은 1 GiB 유지.
- **viva-gpu Metal 드라이버 이식(2026-11-16)**: `github.com/Nik300/viva-gpu`(Mali-G57/iOS 16.7.16에서 UI 전체를 Metal로 그림, GPLv3+저작자
  표시 조건)를 `gpu/viva/build-viva.sh`가 332d26a로 받아 `gpu/viva/vgpu_xc5.c`(viva의 vgpu 선택자 → 우리 XC5GPU 선택자; 동기 제출이라 syncobj·
  완료 페이지는 프로세스 안에서 처리, IOSurface는 plane 0만)를 끼워 VivaMTLDriver.bundle(약 15 MB) 빌드. Valhall ISA 컴파일러는 끔(G52=Bifrost →
  AIR→NIR→Panfrost Bifrost). CI `viva.yml`(macos-15, 산출물 viva-ios). 커널: `xc5mtldrv=viva`면 IOAcceleratorES의 MetalPluginName/Class를
  `../../../private/var/xc5/viva/VivaMTLDriver`/`VMDevice`로(커밋 05e3790). `mkdrop.py --viva DIR`로 드롭(/private/var/xc5/viva, NOTICE/LICENSE
  포함, 패키지 1794826926). 시험 이미지 `xc5-ios-viva1-console.tar`(콘솔 끔, chronod만 Metal, xc5mtldrv=viva). 로그 `XC5GPU client: viva: ...`.
  `XC5_QUIET=1 mkimage.sh` = 화면 콘솔 끔(a5fb_noconsole=1). `xc5metalfor=*`(전부), `-이름`(제외) 지원(커밋 1c...).
- **순정 펌웨어 재확인(2026-11-15)**: `~/xc5/stock/vendor/wifi`(G525NKXSFEYL1) — 우리가 쓰는 mx140.bin·mx140_wlan.hcf·mx140_wlan_sw.hcf는
  md5까지 순정과 **동일**. mx140.bin 빌드 = `S612_E3830_R4-1_Y24W27A`(2024-07-03, `nacho_s612`), platform.txt `nacho_s612_xcover5_kor`.
  **불일치 발견**: `mx140/build_id.txt`와 그 debug 폴더(log-strings.bin, mib_out.xml 등)는 `Y22W51A`(2022-12-19) — 펌웨어보다 오래된 빌드라
  우리 로그 해석 문자열이 안 맞음(예: 늘 마지막 줄 "Speedy drive strength changed"는 잘못 풀린 줄일 수 있음; 원시 오프셋으로만 신뢰).
  순정 커널(boot.img, research/boot.img와 동일)의 **KA 패치 57워드는 우리 ka.bin과 바이트까지 같음**. 순정 커널은 `common.hcf`·`common_sw.hcf`·
  `localmib.hcf`·`bt.hcf`를 찾지만 vendor에는 mx140_bt.hcf만 있어 common/localmib는 없음(우리 fwconfig 0과 같음). 그 밖의 파일: mx140_t.bin
  (시험용 펌웨어), mx140_wlan_t*.hcf, slsi_reg_database.bin(국가 규칙), indoorchannel.info, hydra_config.sdb. → 펌웨어 파일·KA 패치는 원인 아님.
- **사례 조사 2(2026-11-14)**: (1) `github.com/chelovechishee/sm-t510-ubuntu` — Galaxy Tab A 10.1(Exynos 7904, SCSC S612)에서 삼성 SCSC 드라이버를
  **메인라인 Linux 7.3으로 포팅, Wi-Fi 동작**(`mainline/patches/0013`(scsc core)·`0014`(wlan), `mainline/README.md`). 교훈: S-Boot가 안 켜는
  WLBT용 **PMIC LDO 4개를 always-on**으로 해야 함(없으면 -84 dBm·인증 실패 0x800c), 펌웨어 이벤트 로그 MLME_EVENT_LOG_IND, NULL 서비스 시험
  방법. 펌웨어 멈춤 사례는 없음. (2) Exynos9611 커널 PR #16 — mx140 펌웨어는 IGMP 오프로드(MIB 2489)가 기본 켜짐, 받은 IGMP를
  MLME_RECEIVED_FRAME_IND로 호스트에 올림; `slsi_mlme_set`으로 끔(멈춤과 무관, MLME_SET 사용 예). (3) Exynos 850 벤더 커널 트리:
  `XhakaTK/PegasusKernel_Exynos3830`(A12s 5.10), `MRIlla/android_kernel_samsung_a145f`(A14 4G, Android 15 소스 — 더 새 scsc 드라이버·
  platform_mif 비교 가치), `jameskdev/...a21s`. (4) 메인라인: postmarketOS Exynos 850 Wi-Fi "Untested", A13 "broken", exynos850-mainline 목록에
  Wi-Fi 없음 → Exynos 850에서 벤더 드라이버 밖 Wi-Fi 동작 사례는 못 찾음. 다음 확인 후보: a145f 트리의 platform_mif_3830/hip4와 우리 구현 차이,
  Exynos 850 DT의 WLBT용 LDO always-on 여부.
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

- **viva2/viva3 결과(2026-11-16)**: viva2 849 submit 중 실패 2(fragment type 9, status 0x5a = JOB_READ_FAULT, fault_ptr는 정상 BO 안, 원인 미상). viva3(`xc5metalfor=*,-SpringBoard,-backboardd`) 사용자: 앱 다 잘 됨. 다음 viva4 = SpringBoard도 Metal(`*,-backboardd`).
- viva4(SpringBoard도 Metal) 사용자: 잘 됨. 다음 viva5 = 전부(`xc5metalfor=*`, backboardd 포함).
- viva5(`xc5metalfor=*`, backboardd도 Metal): backboardd SIGSEGV(pc 0x104712e44, far 0xcca8 = NULL 근처) 반복, fb swap=0 present=0 → 화면 갱신 없음. 원인 미상(viva가 화면 출력을 AppleCLCD 직접 스캔아웃 전제, 우리는 SweetFB). **기준은 viva4**(`xc5metalfor=*,-backboardd`).
- viva6 PREV BOOT LOG: backboardd SIGSEGV(addr 0xcca8) = VivaMTLDriver panfrost_batch_read_rsrc <- panfrost_get_index_buffer_bounded <- [VMRenderEncoder draw:index:indirect:] block <- VMCommandBuffer execute. 인덱스 버퍼 리소스 경로에서 죽음. 수정(커밋 d8ecc46, CI 43874407595): build-viva.sh가 vmg_exec.c를 패치해 GPU가 쓰지 않는 비-indirect 인덱스 버퍼는 CPU 복사본을 user indices로 넘김. viva7 = 같은 boot-args(xc5metalfor=*), drop 1794835314. 시험 대기.
- viva7 결과: backboardd 크래시 없음, fb swap/present=97, 터치 됨. 하지만 GPU 체인 15% 실패(js_status 0x5a/0x5b, mmu 0) → 잠금화면 검정, 앱 화면 이상. viva8: backboardd 타일러 힙 96 MiB(vgpu_xc5.c, CI 43876945769, drop 1794836174). 시험 대기.
- viva8: 힙 96 MiB 효과 없음. 실패 줄: job status 0xa005a fault_ptr = 작업 자신의 디스크립터 BO 안(mmu 0), type 9(fragment). 추정: CPU 캐시 미반영. viva9: submit 전 CPU 매핑 BO 전부 sys_dcache_flush(VIVA_XC5_NOCLEAN로 끔), CI 44062249222. 시험 대기.
- viva9: flush 효과 없음(jobs 2493 failed 256). 0x5a = ADDR_RANGE_FAULT. viva10: 실패 체인 덤프(프로세스, job 헤더 16워드, FBD 32워드, fault_ptr 위치 16워드, 각 주소의 BO+오프셋), 클라이언트당 3회. flush는 VIVA_XC5_CLEAN일 때만. CI 44283158817. 시험 대기.
- PREV BOOT LOG 필터(loader.c): 기본으로 viva/XC5GPU/CRASH/gpusvc/heartbeat/panic 줄만 표시(FILTER 제목, 파란색). 최신 페이지에서 VOL+ = 전체/필터 전환. viva10 이미지에 포함.
- viva10 덤프: backboardd fragment job(type 9) 0xa005a, fault_ptr = 같은 BO 안 샘플러 테이블(5e888801 1fff0000 = Bifrost sampler 2개). 추정: 셰이더가 바인딩 개수 밖 샘플러 슬롯을 읽음. viva11: 샘플러 32슬롯 전부 바인딩(VMG_NO_SAMPLER_PAD로 끔), CI 44286912215. 시험 대기.
- viva11: 샘플러 32칸 효과 없음(jobs 2524 failed 240). 같은 위치 값 5e888801 1fff0000 0 0 = 크기 0 버퍼 추정(viva12 후보: 빈 버퍼 칸에 더미). 사용자: backboardd Metal 보류, viva4 기준. 다음: 사파리 웹화면 검정/흰색(viva 이전부터). safari1 = viva4 설정 + 필터에 WebKit/Safari/WebContent/sandbox/jetsam.
- safari2: 패닉 = vm_page_insert past object bounds(size 0x842000 = w30r 표면 1125x1905 bpr 4544). 직전 WebKit.GPU SIGSEGV util_copy_box <- vmg_texture_sync: w30r(4B/px)를 rgba16f(8B/px) 텍스처로 복사하며 행 9000B로 표면 끝 넘어감. 수정(build-viva.sh): 행이 pitch보다 크면 복사 생략. safari3 = 이 드라이버, viva4 설정. 별도: xc5net getaddrinfo 8(DNS 실패), locationd 크래시. 시험 대기.
- safari4: w30r(BGR10_XR, mtl 554/555) 표면을 half float로 변환 복사(mapple 85954b3 이식, build-viva.sh), CI 44693189208. 시험 대기.
- safari4: 패닉 없음, fast.com 글씨 보임(DNS 됨), 배경 검정·글씨 뭉개짐. 변환이 mtl_format 554/555 조건에 안 걸린 것으로 판단. safari5: IOSurface 픽셀포맷 'w30r'이면 변환, CI 44695266801. 시험 대기.
- safari5: fast.com 페이지 레이아웃·글씨 그려짐, 배경 검정·글씨 밝은 회색(색 반전처럼 보임). backboardd(hakios-gl)는 w30r을 deep_to_bgra로 이미 변환. safari6 = viva xr 변환 값 로그('vmg: xr in/out ...'), CI 44696933710.
- safari7: 커널 IOSurface가 'w30r'(4B/px) 요청을 'BGRA'로 만듦(hakios 방식, 0046 patch, xnu CI 45148639038). viva/hakios-gl 변환 경로 우회. 시험 대기. (로컬 xnu/patches 130개 삭제 상태 — git엔 있음, CI 영향 없음)
- safari7: w30r 사라짐, 하지만 배경 검정·숫자 가운데 몰림·찢어짐. 원인: WebKit이 여전히 BGR10_XR(554) 텍스처를 요청 → viva가 BGRA 바이트를 10비트로 '변환'. safari8: viva에서 표면 위 550-555 형식을 BGRA8(80/81)로(hakios 방식), CI 45152067085. 시험 대기.
- safari8: 배경 검정 유지, 숫자 겹침(이전 프레임 안 지워짐), 줄 찢어짐. 추정: WebKit.GPU의 clear(배경 흰색 채우기)가 표면 텍스처에 반영 안 됨. safari9 = WebKit.GPU만 viva 제외(비교 시험).
- safari9: 숫자 겹침 사라짐(viva 원인 확정), 라이트 모드인데 Google 페이지 흰 바탕 검정. 추정: WebKit이 w30r로 알고 alpha 0. safari10: 커널이 w30r→BGRA 표면에 여분 3페이지 표시, hakios-gl이 그 표면 alpha=1 swizzle(xnu 45156001220, mesa-ios 45157549287, drop 1795430472). WebKit.GPU viva 제외 유지. 시험 대기.
- **코드 점검·수정(2026-11-26, Claude, 폰 시험 전)**: 전체 점검에서 찾은 것을 고침. 커밋 70c9ca5, **xnu CI 45819049154 성공**(빌드 통과). 시험 이미지는 아직 안 만듦(`gh run download 45819049154 -n xnu-kernel -D xnu/out` 뒤 `mkimage.sh`).
  - Wi-Fi(`xc5_wlbt.c`): 쓰기 결합 공유 메모리에서 소비 쪽 배리어가 없었음 — 펌웨어 쓰기 인덱스를 읽은 뒤 항목·mbulk를 읽기 전, 그리고
    읽기 인덱스를 돌려주기 전에 `dsb sy`(`shm_bar`, TH ctrl/data·FH_RFB·mgmt 링·mxlog). A50(Exynos 9610, 같은 SCSC mx140,
    `nespaces/ios-a50-exynos9610` `core/drivers/wifi/hakios.scsc.c`)도 인덱스 접근 앞뒤에 dsb. `xc5wbar=0`이면 예전처럼.
    "미연결이면 R4가 1~20초 뒤 FH ctrl 큐를 안 읽음" 멈춤의 후보 원인.
  - 제어 슬롯 32칸이 다 바쁘면 펌웨어가 아직 안 읽은 칸을 덮어쓰던 `ctl_force`, 데이터 64칸 0.5초 뒤 강제 회수(`tx_force`)를 끔:
    빈 칸 없으면 안 보냄(셈은 그대로 `force`/`full`). `xc5wforce=1`이면 예전처럼.
  - `read_mgmt`를 Wi-Fi 스레드와 heartbeat가 동시에 돌던 것(START_CFM 유실 가능) → 한쪽만. `ap_sel`을 락 안에서 한 번 읽음(`aps[-1]` 방지),
    PMK는 락 안에서 `wpa_on`과 함께 설치, 커널이 개방형 망에 붙을 때 `wpa_on` 해제. 펌웨어 오프셋 검사를 `ref < SHMEM_MAP - N`(오버플로 없음).
  - 기본 RAM 창(`mkadt_xcover5.py`)이 Wi-Fi 공유 메모리 0x880000000에서 시작하던 것 → 기본 `0x8a2800000:0x40000000`(mkimage와 같음).
    커널도 겹치면 Wi-Fi를 시작하지 않음(`XC5 WLBT: XNU RAM ... covers the firmware's memory`).
  - `xc5_pmic.c`: ACPM 큐 인덱스·크기 범위 검사. `mkimage.sh`: `make-boot.sh` 실패를 숨기던 grep 고침, `XC5_STOCK_BOOT=`. `xnu.yml`: keep_going 식.
  - 이 문서 733-735줄이 CP949였음 → UTF-8로.
  - 시험: 설정 앱 모드(`xc5wifiui=1`)에서 2분 넘게 `q0 r`가 `w`를 따라가고 `stall at 0`, `ctl force 0`인지. 자동 모드(`xc5net=1`) 회귀 없음(ping).
    안 되면 다음 후보(A50 대조): 850 PMU의 MASK_PWR_REQ 대응 비트, 연결 순서 DEL_VIF→ADD_VIF→SET rates/HT→CONNECT.
  - 남은 점검 결과(안 고침): XC5GPU 사용자 클라이언트 접근 제어·GPU VA 재사용 없음, `IOAcceleratorES::setProperties` 누구나, `xc5_drop.c` 허용 pid 미정리,
    WPA2 replay counter·ANonce 검사 없음, 비밀번호가 부팅 인자(`kern.bootargs`), `regen.sh`·`wlbtfw.sh` 견고성.
- **wifi57 이미지(2026-11-26)**: `xc5-ios-wifi57-console.tar`(91,985,920 B, SHA256 `390eb42d...388f54`) = xnu CI 45819049154(70c9ca5: 배리어·슬롯 강제 재사용 끔) + safari10과 같은 인자(`xc5wifiui=1 xc5net=1 xc5nets=...`, viva, `a5fb_noconsole=1`), 드롭 그대로 1795430472. 화면 콘솔이 꺼져 있으니 **PREV BOOT LOG**(필터에 heartbeat)로 볼 것: `XC5 w ctl back force busy | ... q0 w r ... stall at`(force 0, r가 w를 따라가고 stall at 0이면 멈춤 해결), 설정 앱 Wi-Fi 연결·체크, 부팅마다 연결되는지. 새 경고 줄 `XC5 WLBT: XNU RAM ... covers`가 나오면 RAM 창 문제. 비교: 같은 이미지에 `xc5wbar=0`이면 예전 동작.
- **wifi57 결과(PREV BOOT LOG 사진 19장, 2026-11-26)**: 펌웨어가 **훨씬 오래 삶** — `done 84 chain 83`, `ka 545/544`, `got 19760`, `rfb 39171 chained 19411 full 0`, `ctl back 640 force 0 busy 200080`, `tx back 28 full 0 force 0`(강제 재사용 0), **`stall at 151 s`**(전에는 거의 늘 31~45 s, ka 최대 66). 그래도 결국 `q0 w130 r129`, `why st 10 sr1 20 msr1 20 issr0 2222001b logw 203 recs 58 kick 65/0 pan 0 0`, wdog/alive 0 — 같은 멈춤 모양(도어벨 미처리). → 배리어 수정은 효과가 큼(수명 약 4배)이지만 근본 해결은 아님. 연결: `shadow tries 1 ok 1 used 0`, `ap S24 2437 -10 o`, `join cfm 1 ind ffff st 0 disc 3 n 1`, `data tx 28 rx 2 dhcp 1 offer 0.0.0.0`, `wpa 0 ... m1 1 m3 1 keys 2 res 0`, `ui join assoc 1 why 4 key 0 | stage 5 err 60 | cfm 1`. 즉 커널 선연결은 됐다가 DHCP offer 없이 끊김(disc 3 = 떠남, 설정 앱이 다른 조건으로 연결 요청 → `same` 거짓 → 우리 DISCONNECT 경로로 추정, 미확인), 설정 앱 연결은 CONNECT_CFM 1(거부)·25초 시간 초과, 그 뒤 151 s 멈춤. GPU 정상(`gpusvc jobs 6909 failed 0`), locationd SIGTRAP 반복(별건). 다음 후보: (1) A50처럼 PMU 전원 요청 붙잡기(9610 WLBT_CTRL_NS bit18 MASK_PWR_REQ — 850 대응 비트를 A21s platform_mif_3830.c에서 확인), (2) 설정 앱 요청이 선연결과 왜 안 맞는지 진단 줄(`same` 조건별), (3) 선연결 뒤 DHCP offer가 안 온 이유(rx 2).
- **wifi58(2026-11-26)**: A50의 MASK_PWR_REQ는 3830(mif_reg_S5E3830.h)에 없고 삼성 3830 전원 순서는 우리와 같음 → 대신 진단. 커밋 bfb0913, xnu CI 45823101133. 멈춤 때와 하트비트에 PMU `WLBT_IN`([2] PWRDOWN_IND, [0] SWEEPER ACK) `WLBT_OUT`([7] SWEEPER 요청, [13] bypass) `WLBT_STAT`([0] PWRDN_DONE, [4] ACCESS_MIF): 콘솔 `XC5 WLBT: FW STALL ... | in out stat`, 하트비트 `XC5 w pmu2 in out stat | at stall in out stat`. 이미지 `xc5-ios-wifi58-console.tar`(wifi57과 같은 인자·드롭, SHA256 ff29c6b1...e17151). PWRDOWN_IND=1이면 절전에서 못 깨어남, ACCESS_MIF=1(전원 켜짐)이면 메모리 버스에 걸림 추정.
- **wifi59(2026-11-26, 사용자 요청: 로그에 필요한 것만)**: 커밋 657d155(로그 정리), xnu CI 45824980121. exec 줄은 이름마다 3번(xpcproxy는 안 찍음), 같은 프로세스 크래시는 2번 전체 뒤 `CRASH name[pid] sig N again (N times)` 한 줄(10번까지, 그 뒤 20번마다), apple80211 요청은 같은 답이면 처음 2번만(SCAN/JOIN set은 항상); `xc5logall=1`이면 전부. PREV BOOT LOG 필터(기본으로 열림) = `FILTER (Wi-Fi/CRASH)`: XC5 w/WLBT/wlbt/hip/FW, xc5net, CRASH, panic, heartbeat, gpusvc, viva. 이미지 `xc5-ios-wifi59-console.tar`(wifi58 진단 포함, 같은 인자·드롭, SHA256 ad87c8ba...b49bd).
