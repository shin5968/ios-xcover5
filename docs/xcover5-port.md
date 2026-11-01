# Galaxy XCover5 (SM-G525F/N, Exynos 850) 포트

ios-galaxy-s8(Galaxy S8 포트)과 ios-sweet(원본 Redmi Note 10 Pro 포트)를 바탕으로
iOS 16.7 userland를 Galaxy XCover5에서 부팅하려는 포트입니다.

> ⚠️ **아직 기기에서 한 번도 실행해 보지 않았습니다.** 아래 "하드웨어 사실"은 모두
> 순정 펌웨어(G525NKXSFEYL1)의 디바이스 트리에서 읽은 값이고, 부팅 결과는 없습니다.
> 1단계(프로브)부터 순서대로 시험하고, 결과(화면 사진)를 이 문서에 기록하세요.
> 설치하면 **USERDATA(안드로이드 데이터 전체)가 지워집니다.** 부트로더 언락이 필요합니다.

## 기기 결과

### 프로브 1 (2026-11-01, SM-G525N, Magisk boot 헤더 기반 `xc5-probe-boot.tar`)

Odin으로 BOOT에 플래시 → S-Boot가 gzip 로더를 실행, 화면 출력 정상(XRGB8888 가정 맞음).

| 값 | 의미 |
| --- | --- |
| `EL=0x1 SCTLR=0xc5183a` | EL1, MMU 꺼짐(M=0), D-캐시 꺼짐(C=0), I-캐시 켜짐, 정렬 검사(A=1) 켜짐 |
| `DTB=0x8a080000` | S8과 거의 같은 위치 |
| `loader=0x80141eac` | 로더는 `0x80080000` 근처에 적재(stock 커널 위치) |
| `MIDR=0x410fd051` | Cortex-A55 r0p1 |
| `MPIDR=0x81000000` | 예상대로: MT 비트, 부트 코어 = cluster 0 core 0 → XNU 키 `0x000` |
| `MMFR0=0x101122` | 4K/16K/64K 페이지 모두 지원, PA 40비트 |
| `ISAR0=0x10211120` | LSE 원자 연산 있음(A53용 빌드 플래그와도 호환) |
| `PFR0=0x10112222` | GIC 시스템 레지스터 없음 → GICv2(GIC-400) 확인 |
| `CNTFRQ=0x18cba80` | 26 MHz가 설정되어 있음(S8은 0이었음) |
| `initrd=0x84080000 end=0x84081000` | 램디스크 4096바이트 그대로, end는 exclusive |

결론: 로더 진입 조건과 화면 주소는 S8 경로와 호환. 다음은 커널(CI 빌드)과 RAM 로그 유지 확인.

### iOS 부팅 1 (2026-11-01, CI run 41286409991 커널, `xc5-ios-boot.tar` + Odin으로 쓴 `xc5-ios-userdata.tar`)

Odin이 HFS+ sparse userdata 이미지를 받아들임. XNU 진입, 화면 콘솔 동작(줄이 화면 끝에서
위로 되감김). 사진에서 확인한 마지막 단계: `iBoot version: xcover5-entry`, ulock 초기화,
SWEET PRNG, A5XELTE pthread/AMFI, vnguard, 플랫폼 익스퍼트, AppleKeyStore/IOWatchdog/
IOCoreSurfaceRoot/HID(버튼·터치 서비스)/배터리(정적 100 %), `SWEET FB: AppleCLCD 720x1480
stride 2880 at 0xfa000000`. 이 다음(eMMC 루트, launchd)은 아직 보지 못함.

### 부팅 2–5 (2026-11-01)

- 부팅 2: AppleCLCD 뒤 무출력. RAM 로그 뷰어는 동작(강제 재시작 후 RAM 유지).
- 부팅 3 (heartbeat 커널, run 41292039523): 5초 간격 = 130.03M 틱 → 26 MHz 물리 타이머
  인터럽트 정상. CPU 드라이버 줄 없음 → SweetCPU가 `arm,cortex-a53`만 매칭, ADT 수정.
- 부팅 4: CPU0 `1/1 CPUs initialized, SGI1 verified`, BSD/Skywalk/ifnet, 키, eMMC 호스트
  (`ctrl=10 status=8906 clk=1 hcon=8e42541`), GPT CRC 통과, 그러나 `USERDATA partition not found`.
- 부팅 5 (run 41294736935, 대소문자 무시): `GPT USERDATA first=14321664 sectors=107806720
  writable`, `BSD root: emmcroot0`, `hfs: mounted iOSRoot`, `exec launchd`,
  `shared_region ... vm_shared_region_start_address() failed`, 이후 1분간 heartbeat만.
- 부팅 6 (eMMC 카운터): `emmc rd=2232`에서 정지 → 디스크 유휴, launchd 첫 시스템 콜에서 멈춤.
  원인: A55는 PAN이 있는데 SCTLR_EL1.SPAN=0 → 예외 진입마다 PAN=1, 커널(0095)이 끄지 않음.
- 부팅 7 (run 41299006129, SPAN=1): launchd 동작, 데몬 다수 exec(backboardd, notifyd,
  containermanagerd, bluetoothd, securityd, lockdownd, ...). heartbeat 126(약 10.5분)에
  `emmc rd=2711512 wr=591381`. 두 번째 영상(20261101_200425)에서 **iOS 사과 로고가 화면에
  여러 번 그려짐**(heartbeat 81 부근 등): backboardd → AppleCLCD → 0xfa000000 경로로 iOS
  그래픽 출력 확인. 콘솔 글자가 그 위를 다시 덮음 → 다음은 `a5fb_noconsole=1`.
- 부팅 8 (`a5fb_noconsole=1 a5fb_keep_on=1`, 영상 20261101_204853): 콘솔이 멈추고 사과 로고 유지 →
  **iOS 잠금화면**(`4:29`, `Wednesday, December 31`, 홈 인디케이터, 알림 배너). 화면이 붉고
  `rdar:45025538` 막대: UIKit "no status bar visual provider"(240x493 pt는 실제 아이폰 크기가 아님,
  sweet NOTES 0061과 동일). 날짜는 RTC 없음. → AppleCLCD가 1125x2436을 보고하고 패널에 축소 출력.

## S8 포트와 다른 점

| 항목 | Galaxy S8 (dreamlte) | XCover5 (이 포트) |
| --- | --- | --- |
| SoC / CPU | Exynos 8895, Cortex-A53 ×4 (+ Mongoose) | Exynos 850 (s5e3830), Cortex-A55 ×8 (클러스터 2개) |
| MPIDR | Aff0 = 코어 (0x100–0x103) | MT 비트, **Aff1 = 코어**, Aff2 = 클러스터 (0x000–0x300, 0x10000–0x10300) |
| 저장장치 | UFS (`sweet_ufs.c`) | **eMMC** DesignWare MSHC `dwmmc0@12100000` → A5용 `a5xelte_emmc.c` 재사용 |
| 화면 | 1440×2960, command mode (DECON 트리거 필요) | **720×1480**, NT36525 video mode, fb `0xfa000000` |
| GIC | GIC-400 `0x10201000/0x10202000` | GIC-400 `0x12a01000/0x12a02000` |
| 타이머 | 26 MHz, physical timer (INTID 30) | 동일 (DT `use-physical-timer`) |
| 터치 | S6SY761 (I²C) | Novatek NT36525 (**SPI**) — 드라이버 없음 |
| 버튼 | GPIO: 전원/볼륨/빅스비 | GPIO: 볼륨 위/아래, XCover 키. 전원 키는 PMIC(S2MPU12)라 미지원 |
| 로그 | TWRP ramoops | ramoops 없음 → 로더 RAM 로그 + 로그 뷰어(아래) |
| 부트 이미지 | boot v0 + DTBH | **boot header v2** + Exynos dt_table + AVB 푸터 |

## 하드웨어 사실 (순정 DT에서 읽은 값)

출처: `boot.img`(header v2)의 dtb, `dtbo.img`, `XCOVER5_KOR_OPEN.pit`, `sboot.bin` 문자열.

- 부트 체인: S-Boot(`sboot.bin`, LK 아님) → BOOT 파티션의 header v2 이미지. 커널은 gzip
  Image (`Linux 5.10.223-android13`), 커널 cmdline `androidboot.hardware=s5e3830`.
  S-Boot 문자열에 `Could not do normal boot (decompression failure)`,
  `Custom binary blocked by OEM Lock`가 있음 → 언락 필요, 커널은 gzip으로 넣음.
- RAM: `memory@80000000`(2 GiB), `memory@880000000`(2 GiB). 상위 뱅크의 예약 영역은
  `mfc_fw_rmem 0x8a2400000` 하나뿐 → XNU는 `0x880000000`부터 512 MiB 사용.
- 하위 뱅크 예약: ect `0x90000000`, el3mon `0xbff80000`, seclog `0xc3000000`,
  modem `0xd0000000`, gnss `0xee000000`, debug_snapshot `0xf0000000`,
  **bootloader_fb `0xfa000000` (20 MiB)**, initrd `0x84000000`.
- GIC-400: GICD `0x12a01000`, GICC `0x12a02000`. 타이머 26 MHz, PPI 13/14/11/10.
- CPU: `cpu@0..3` reg `0x000..0x300`, `cpu@4..7` reg `0x10000..0x10300`, PSCI 1.0 (smc).
- 화면: `decon_f@0x13060000`(psr_mode 0 = video mode), 패널 `nt36525_xcover5`
  720×1480 60 Hz, DSI 4 lane.
- eMMC: `dwmmc0@12100000`, fifo-depth 64, 8-bit, HS400 ES, CQE/FMP(인라인 암호화) 지원.
- 파티션(PIT, eMMC 섹터): BOOT p18 (44 MiB), CACHE p34 (200 MiB), USERDATA p38(나머지 전부).
  이름은 대문자(`BOOT`, `USERDATA`, `CACHE`).
- 키: 볼륨 다운 `gpa1-0`, 볼륨 업 `gpa0-7`, XCover 키 `gpa0-5`(KEY_HOT). ALIVE pinctrl `0x11850000`.
- 터치: `novatek,NVT-ts-spi` on `spi_1`, IRQ `gpa0-0`, CS `gpm3`.

## 무엇을 만들었나

| 경로 | 내용 |
| --- | --- |
| `shim/xcover5/` | S-Boot용 로더 (S8 로더 기반). 프로브 모드, RAM 로그, 로그 뷰어 |
| `tools/xcover5/mkboot.py` | boot header v2 패커 (순정 헤더/DTB/vbmeta 재사용, Magisk와 같은 배치) + Odin tar |
| `tools/xcover5/make-boot.sh` | 커널 → ADT → payload → BOOT 이미지 한 번에 |
| `tools/mkadt_xcover5.py` | XCover5 Apple Device Tree |
| `tools/xcover5/apply-xcover5.py` | S8 xnu 트리 → XCover5 (패치 0125 생성기) |
| `xnu/patches/0125-xcover5.patch` | `XCOVER5` 정의: fb/로그/MPIDR/키/eMMC 주소, SMP 트램펄린 |

`0125`는 `0124`가 정의하던 `DREAMLTE` 대신 `XCOVER5`를 정의합니다. 따라서 이 브랜치의
커널은 XCover5 전용입니다. 0001–0125 전체가 xnu-8796.141.3에 `git apply`로 깨끗이
적용되는 것을 로컬에서 확인했습니다(CI 스테이징이 필요한 0031/0041 제외). **커널 빌드
자체는 macOS CI에서만 가능하며 아직 돌려보지 않았습니다.**

## 설치 절차

### 0. 준비

- 부트로더 언락된 SM-G525N/F (Magisk가 이미 돌아가면 언락 상태).
- PC: Windows + Odin(3.14.4), WSL Ubuntu(`gcc-aarch64-linux-gnu`, `python3`).
- **지금 쓰고 있는 BOOT 이미지 백업** (순정이든 Magisk든, 이것으로 복구합니다).
  `boot_phone.img`가 이미 있다면 그것을 사용합니다. 순정 펌웨어의 `boot.img.lz4`도 있습니다.

### 1단계: 프로브 (안전, BOOT만 바꿈)

로더가 S-Boot에서 정상적으로 실행되는지, 진입 상태가 예상과 같은지 확인합니다.

```bash
bash tools/xcover5/make-boot.sh --probe ../boot_phone.img shim/xcover5/out/xc5-probe-boot.img
```

Odin → AP에 `shim/xcover5/out/xc5-probe-boot.tar`, Auto Reboot 끄지 말고 Start.
(다운로드 모드: 전원 끈 상태에서 볼륨 위+아래를 누른 채 USB 연결)

예상 화면: 맨 위 파란 줄, 노란 `XC5LOADER BEGIN`, 그 아래 `EL=0x1 ... MPIDR=0x81000000`,
`initrd=0x84000000 ...`, 초록색 `PROBE OK`. **이 화면 사진이 다음 단계의 근거입니다.**
아무것도 안 나오면: 화면 포맷/주소가 다르거나 S-Boot가 이미지를 거부한 것입니다.

복구: 다운로드 모드에서 원래 BOOT를 tar로 묶어 AP로 플래시
(`python tools/xcover5/mkboot.py`를 쓰지 말고 원본 그대로: `tar -cf boot.tar boot.img`).

### 2단계: XNU 커널 빌드 (GitHub Actions)

이 디렉터리를 GitHub에 push(브랜치 `xcover5`)하면 `xnu` 워크플로가 돈다(machine_config
`A5XELTE`). 결과 `kernel.development.a5xelte`를 받습니다.

```bash
gh run download RUN_ID -R YOU/ios-xcover5 -n xnu-kernel -D xnu/out
```

### 3단계: iOS 루트 이미지

S8 문서(`docs/s8-install.md` 3절)와 같습니다. iPhone X(iPhone10,3) iOS 16.7.16(20H392)
IPSW는 직접 받아야 합니다. `research/ipsw-x/d22ap.adt`도 같은 방법으로 준비합니다.

```bash
sudo tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G
sudo tools/dreamlte/finish-iosroot.sh ios16-root.img
img2simg ios16-root.img userdata.img          # Android sparse
tar -cf ios-userdata.tar userdata.img          # Odin AP용 (크면 lz4 없이도 됨)
```

USERDATA에 쓰는 방법(둘 중 하나, **안드로이드 데이터가 모두 지워짐**):
- Odin: AP에 `ios-userdata.tar`. 순정 펌웨어가 `userdata.img`를 Odin으로 쓰는 것과 같은 경로지만,
  HFS+ 이미지를 이 방식으로 쓴 사례는 아직 없습니다.
- 루트 셸이 있는 리커버리(TWRP 등)가 있다면 `dd`로 `/dev/block/by-name/USERDATA`.

### 4단계: XNU BOOT 이미지

```bash
bash tools/xcover5/make-boot.sh xnu/out/kernel.development.a5xelte ../boot_phone.img shim/xcover5/out/xc5-ios-boot.img
```

기본 boot-args: `cpus=1 rd=emmcroot0 emmcroot=USERDATA emmcwrite=1 rootrw=1 wdt=-1 ...`.
루트 이미지 없이 커널만 시험하려면 `rd=emmcroot0`을 빼고 넣으면 루트 대기에서 멈춥니다(정상).
Odin AP에 `xc5-ios-boot.tar`.

### 로그 보기 (TWRP 없이)

로더와 커널은 RAM(`0xb0000000`, `0xb0080000`)에 텍스트 로그를 남깁니다.
멈추거나 패닉이 나면: **전원 + 볼륨 다운을 7초** 눌러 강제 재시작하고, **볼륨 다운을 계속 누르고
있으면** 로더가 부팅 대신 지난 부팅의 로그를 보여 줍니다. 볼륨 업 = 이전, 볼륨 다운 = 다음,
XCover 키 = 그냥 부팅. (강제 재시작에서 RAM이 유지되는지는 아직 확인 전입니다.)

`emmcwrite=1 a5log=1`이면 커널 로그를 CACHE 파티션 앞 4 MiB에도 씁니다(헤더 `A5XLOG1`).

## 알려진 위험과 미구현

- 로더 진입 상태(EL1, MMU off, x0=DTB)는 S8의 S-Boot에서 측정한 값입니다. Exynos 850의
  S-Boot에서 다를 수 있습니다 → 1단계가 먼저입니다.
- 픽셀 포맷: 로더는 XRGB8888을 가정합니다. 색이 뒤바뀌어 보여도 글자가 보이면 정상입니다.
- eMMC: S-Boot가 남긴 클럭/타이밍을 그대로 쓰는 PIO 드라이버입니다. CQE가 켜져 있거나
  하드웨어 클럭 게이팅(`card-init-hwacg-ctrl`)이 동작하면 실패할 수 있습니다(로그에
  `host unavailable` 또는 `CMD17 error`).
- iOS 화면 크기: 720×1480 그대로(native) 보고합니다. iPhone X(1125×2436) 레이아웃은 S8에서
  첫 프레임에 멈췄으므로 쓰지 않습니다. UI가 크게 보이거나 잘릴 수 있습니다.
- 멀티코어: `cpus=1`만 권장. `cpus=2..4`는 저위 트램펄린(`0xb0100000`) + PSCI로 시도하지만
  미검증. 두 번째 클러스터(코어 4–7)는 XNU의 MPIDR 키가 겹쳐 쓸 수 없습니다.
- 터치(NT36525 SPI), 전원 키(PMIC), RTC(PMIC, 날짜는 1970), 배터리, Wi-Fi, 소리, 카메라 없음.
- GPU 없음: 모든 렌더링이 CPU.
