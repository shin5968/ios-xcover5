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
- 부팅 9–12 (2026-11-01): 고속 eMMC(multi-block)로 사과 로고까지 약 1분 40초. iPhone X 크기(gui)는 로고/스피너에서 멈춤
  (S8 README의 "1125x2436 hangs"와 같은 증상). 첫 multi-block 코드가 HTO(0x400)를 오류로 보고 CMD12 없이 끊어
  HFS 손상 → HTO 무시 + CMD12 복구로 수정(run 41309423474). userdata 재플래시 후 native 첫 부팅: 잠금화면 `4:25`
  (첫 부팅은 약 25분, 이미 부팅한 루트는 약 2분). 반복 크래시: locationd(sig 5, GPS 없음), mediaremoted/contextstored
  (sig 11, 공유 캐시 0x20da18040 쓰기) — 깨끗한 루트에서도 동일, 잠금화면을 막지는 않음.
- 부팅 13 (native, 두 번째 부팅): 잠금화면 `4:02` = 약 2분.
- 부팅 14 (`xc5-ios-gui.tar`, 같은 루트): **iPhone X 크기(1125x2436)를 683x1480으로 축소해 잠금화면 `4:02`**.
  붉은 `rdar:45025538` 배너 사라지고 상태 막대(충전 중 배터리 아이콘) 표시. 앞서 gui가 "멈춘" 것은 새 루트의 첫 부팅
  (약 25분)이었음. 남은 것: 화면 위쪽/오른쪽에 콘솔 글자 잔상, 시계 글자 색이 붉게 보임(확인 필요).

### 터치와 홈 화면 (2026-11-02)

- 터치: Novatek NT36525(TDDI), SPI1 = USI_CMGP0 `0x11d00000`, CS gpm3-0, IRQ gpa0-0. 컨트롤러 nCS를 켜야 전송됨.
  칩 ID `1c ff ff 25 65 03` → 삼성 커널 트림 표에서 **NT36672A 메모리 맵 + hw CRC**(이벤트 버퍼 `0x21c00`,
  BOOT_RDY `0x3f10d`). 펌웨어 `vendor/firmware/nt36525_xcover5.bin`(git에 넣지 않음)을 로더가 매 부팅 다운로드
  (`reset state=0xa1`, ILM CRC 일치, fw_ver 0x0e) → `xc5touch=1` → 커널이 10 ms 폴링, 5회 연속 미검출 시 뗌.
- 부팅 16 (CI run 41356829634): **잠금화면을 밀어 iOS 홈 화면 진입**(Photos, App Store, Settings, 독). 약 2분 부팅.
- CI: macos-14 러너 폐기(2026-11-02) → macos-15(Xcode 16, 패치 0083의 IORPC.kernelContent).
- 부팅 17–21 (2026-11-02): cpus=4 토폴로지 패닉(A55 MPIDR Aff1=코어) → 클러스터 0 하나로 수정, 4/4 CPU 기동 확인.
  잠금화면이 안 열린 원인 두 가지: SpringBoard가 AKS 8번을 무한 반복(미지원 → 성공으로 응답), 터치 이벤트 버퍼를
  1바이트 밀려 파싱(벤더 point[1+6i] = 우리 pt[6i]). 수정 커널(CI run 41433557070)로 **밀어서 잠금 해제 성공**.
- 터치 스레드를 CPU0에 고정하면 cpus=1에서도 잠금 해제가 안 됨 → 고정하지 않는 것이 기본.
- 부팅 22 (2026-11-03, cpus=4, 모든 코어 PSTATE.PAN 해제, CI run 41618618719): 잠금화면이 cpus=1과 같은 모습
  (충전 배터리 아이콘, `4:02`). 4코어에서도 이전 부팅 RAM 로그가 남음. 터치는 SweetHID까지 오지만
  heartbeat 75→88(약 65초) 동안 fb swap 14→16, present 13 그대로 → 화면 갱신이 멈춤, 잠금 해제 안 됨.
  locationd/WirelessRadioManager sig 5 반복은 cpus=1과 같음. 다음: 코어별 IRQ 수와 실행 중 작업을 heartbeat에 기록.
- 부팅 23 (cpus=4, CI run 41622079432): 4개 코어 모두 5초마다 타이머 약 1150회, IPI 약 4400회(irq=0)를 받고
  실행 작업도 계속 바뀜 → CPU 멈춤이나 인터럽트 문제 아님. fb swap 16에서 정지 = backboardd가 프레임을 더 내지 않음.
  SpringBoard가 AKS 26/27/8을 빠르게 반복(1000번에 1줄 로그가 연달아 찍힘). 다음: AKS 호출 수와
  SpringBoard/backboardd 스레드 상태를 heartbeat에 기록해 cpus=1과 비교(CI run 41626090600).
- 부팅 24 (cpus=1): AKS 26/27=8회, 8=3회에서 멈춤(반복 없음), 잠금 해제 후 fb swap 46.
- 부팅 25 (cpus=4, `xc5conrows=40` 화면 콘솔, 영상 20261103_210150): 4코어는 재시작 후 RAM 로그가 남지 않음
  (2회 연속). 이번엔 AKS 26/27=6회로 반복 없음 → AKS 반복은 원인이 아니라 증상. fb swap/present 15에서 정지.
  SpringBoard 스레드 3개(tid 1693/1821/1824)가 st=0x49(WAIT|UNINT|WAIT_REPORT), hint 0, 사용자 pc가 시스템콜
  스텁이 아닌 프레임워크 코드 → 페이지 폴트 등 커널 안에서 끊을 수 없는 대기로 추정(cpus=1에서는 없음).
  다음: 해당 스레드의 커널 반환 주소(unslid)와 wait event 기록(CI run 41633066179).
- 부팅 26 (cpus=4, CI run 41634287581, 영상 20261103_213007): fb swap/present 13에서 정지. SpringBoard 2개와
  backboardd 1개 스레드(pri 4, st=0x49)의 커널 역추적이 모두 같음:
  `machine_switch_context ← thread_invoke ← thread_block_reason ← throttle_lowpri_io ← vm_fault_internal`.
  → 백그라운드 우선순위 스레드가 페이지 폴트 중 저우선순위 I/O 스로틀에 걸려 깨어나지 못함. eMMC는 PIO로 한 번에
  한 요청만 처리하고, 크래시를 반복하는 데몬(locationd, WirelessRadioManager, ReportCrash)이 계속 I/O를 내서
  스로틀이 풀리지 않는 것으로 추정. 수정: `lowpri_throttle_enabled = 0` (boot-arg `xc5throttle=1`로 되돌림),
  CI run 41637215054.
- 부팅 27 (cpus=4 console, 영상 20261103_215855): 그대로 swap 14에서 정지, SpringBoard tid 1797/backboardd tid 1530
  (pri 4)이 여전히 `throttle_lowpri_io+0x35c`. 시스템 오버라이드 해제(kern_overrides.c → sys_override_io_throttle)가
  플래그를 1로 되돌림. 수정: XCOVER5에서는 해제 시 부팅 값 유지, 플래그가 0이면 throttle_lowpri_io가 바로 반환
  (CI run 41641434949).
- 부팅 28 (cpus=4 console, 영상 20261103_222048): 여전히 swap 13, SpringBoard tid 1798/1865, backboardd tid 1553이
  `throttle_lowpri_io+0x344`(새 검사 뒤의 sleep). 플래그가 부팅 뒤 다시 1이 되는 경로가 더 있음(누가 바꾸는지 미확인).
  수정: 플래그와 상관없이 boot-arg `xc5throttle`만으로 결정, heartbeat에 플래그 값 출력(CI run 41643952043).
- 부팅 29 (cpus=4 console, 영상 20261103_223642): `throttle=1`(부팅 뒤 누군가 다시 켬). throttle_lowpri_io 대기는
  사라짐. 그래도 fb swap 12에서 정지. SpringBoard tid 1721/1727, backboardd tid 964/1246(pri 4)이 st=4(실행 가능)인데
  heartbeat 34→58(약 2분) 동안 pc가 그대로 → 실행 대기열에서 CPU를 받지 못함(기아). 한 번 lck_mtx_lock_contended 대기
  (SpringBoard tid 1726, pri 46)도 보임. 다음: 실행 가능 스레드의 마지막 실행 후 경과, 대기열/바인딩 CPU, 각 CPU 상태와
  현재 우선순위(CI run 41647468196).
- 부팅 30 (cpus=4 console, 영상 20261103_230240): fb swap 12 정지. SpringBoard tid 1708(base 4, timeshare)
  `age` 169808→179096 ms, runq=2, 바인딩 없음; backboardd tid 1481 age 138 s, runq=0. 모든 CPU가 st=6(실행 중),
  pri 31~46 사용자 작업(ReportCrash, locationd, …)과 heartbeat 자신(kernel_task, pri 81)을 실행 → CPU 포화로 base 4
  스레드가 굶음(추정). 부하 원인 후보: locationd/WirelessRadioManager 크래시 반복과 ReportCrash("Too many corpses").
  시험: `-no_corpses` boot-arg 이미지(커널 그대로).
- 부팅 31 (cpus=4 console, `-no_corpses`, 영상 20261103_231942): corpse 생성은 꺼짐("corpse is disabled by
  config")이지만 fb swap 14 정지. CPU는 다른 pri 4 스레드(launchd, accountsd, rtcreportingd)는 실행하는데
  SpringBoard tid 1657/1693/1695(base 4) age 74→114 s, backboardd tid 1030 age 125 s → 단순 포화가 아니라 clutch
  스케줄러가 특정 스레드 그룹을 건너뜀(추정). 수정: A5XELTE에서 CONFIG_SCHED_CLUTCH를 끄고 dualq 스케줄러 사용
  (CI run 41653549707).
- 부팅 32 (cpus=4 console, dualq, 영상 20261104_183455): 굶는 스레드 없음(run age 수 ms~18 s). 그래도 fb swap 16 정지.
  backboardd tid 304가 `lck_mtx_lock_contended ← emmc_strategy`(eMMC 잠금 대기). emmc rd가 heartbeat 36→56(100 s)
  동안 7.33M→11.70M 섹터(약 22 MB/s 연속 읽기, cpus=1 부팅 24는 heartbeat 70까지 총 5.9M) → 512 MiB RAM에서 페이지
  스래싱으로 추정. 시험: `XC5_DRAM=0x8a2800000:0x5d800000`(MFC 영역 위 1496 MiB) 이미지 `*-1g5`.
- 부팅 33 (`*-1g5`, 1496 MiB): 92726 페이지 초기화 후 패닉 `kmem_range_init: vm_map_locate_space failing for claim
  kmem_ptr_range_meta @vm_kern.c:3645` → 4K 페이지 A5XELTE 커널의 커널 가상 주소 공간이 이 RAM 크기에 비해 작음.
  다음 시험: 1 GiB(`0x8a2800000:0x40000000`) 이미지 `xc5-ios-smp4-1g*`.
- 부팅 34 (2026-11-04, `xc5-ios-smp4-1g-console.tar`: cpus=4, dualq, 1 GiB, CI run 41653549707 커널, 영상
  20261104_185032): **4코어로 잠금화면 → 밀어서 홈 화면(Photos/App Store/Settings) → 제어 센터까지 동작**
  (배터리 100% 충전 표시). 4코어 문제는 세 가지가 겹친 것: 보조 코어 PAN, clutch 스케줄러의 기아,
  512 MiB RAM에서 페이지 스래싱(eMMC 잠금 대기). locationd/WirelessRadioManager 크래시 반복은 그대로(GPS/모뎀 없음).
- 부팅 35 (2026-11-04, `xc5-ios-smp8-console.tar`: cpus=8, 1 GiB, CI run 41811040652, 영상 20261104_193551):
  클러스터 1(MPIDR 0x10000-0x10300)을 키 0x001-0x301로 기동. heartbeat에 cpu0-cpu7 모두 타이머/IPI 수가 늘고
  작업을 실행(locationd, ReportCrash, vmd 등). **8코어로 홈 화면 도달, 시계 4:02→4:04 갱신.** (UI가 한국어로 표시됨.)
- 4 GiB 전체: RAM이 두 뱅크(0x80000000, 0x880000000)로 나뉘고 하위 뱅크에 모뎀/GNSS/EL3/fb 예약 영역이 있으며
  XNU는 연속 영역 하나만 받음 → 연속 최대는 MFC 위 1496 MiB.
- 부팅 36-38 (1496 MiB): `kmem_range_init` 패닉. kmem claim을 1/16, 1/2로 줄이고 가장 큰 구멍(0xfffffff068000000,
  46 GiB)부터 배치해도 매번 다른 claim(compressor, data, ptr_range_meta)에서 실패. 절반 크기 커널은 1 GiB에서도 실패.
  → 공간 부족이 아님. 원래 배치로 되돌리고 패닉에 claim 크기/힌트/kr/전체 목록을 출력(CI run 41823429418).
- 같은 커널: locationd/WirelessRadioManagerd exec 거부(`xc5noexec=0`으로 해제), eMMC 명령당 256 KiB,
  이미지에 `sweetfb_hz=24`. BearSSL은 bearssl.org 502로 `xnu/patches/vendor/`에 사본을 넣어 CI에서 사용.
- 부팅 39: exec 거부 경로가 `kern_exec.c:7593 signature_failure_reason != OS_REASON_NULL` 단언으로 패닉 → 이유 추가.
- 부팅 40 (userdata 재플래시 후 첫 부팅, cpus=8, 1 GiB): 약 16분(이전 25분). 잠금화면에서 밀어도 안 열림.
  heartbeat `dma=0`. 부팅 41 (같은 커널, `xc5noexec=0`): **홈 화면/위젯 화면 정상** → locationd 등 거부가 잠금 해제를
  막음. `xc5noexec`는 기본 꺼짐으로 변경. DMA: `on=0 err=3 last=60`(ETIMEDOUT) — 모든 디스크립터에 DIC를 켜서
  IDSTS.RI가 오지 않음 → 마지막 디스크립터만 인터럽트(CI run 41834474116). 오류 시 레지스터를 heartbeat에 출력.
- 부팅 42 (CI run 41834474116): 여전히 `err=3 last=60`, `rint=402c ids=0 tbb=1000` → 전송은 끝나고 IDSTS만 0.
  완료 판정을 DTO+CMD done+TBBCNT=전체+디스크립터 OWN 반환으로 변경(CI run 41836897933).
  이 부팅은 잠금 해제가 안 됐지만 같은 이미지로 다시 부팅하니 됨 → 잠금 해제 실패는 간헐적(원인 미확인).
- 부팅 43 (CI run 41836897933, 영상 20261104_223221): **eMMC DMA 동작.** `XC5 dma on=1 err=0 last=0 checks=0`
  (PIO 비교 64회 모두 일치), heartbeat 26에서 `rd=4116858 dma=4115440`(읽기 거의 전부 DMA). 홈 화면 정상.
- 부팅 44 (GPU 0단계 `xc5gpu=2`, CI run 41841584580, 영상 20261104_230341): `pd-g3d config=1 status=1 (on)`,
  **GPU_ID 0x72120000(Mali-G52)**, L2_FEATURES 0x7100206, TILER_FEATURES 0x809, MEM_FEATURES 1, MMU_FEATURES 0x2830
  (VA 48, PA 40비트), AS_PRESENT 0xff, JS_PRESENT 7, 셰이더 1 / 타일러 1 / L2 1, ready 0/0/0, COHERENCY_FEATURES 0.
  → S-Boot가 G3D 전원과 클럭을 켜 둠(1단계 불필요). 코어는 꺼진 상태. 다음: 2단계(`xc5gpu=3`, CI run 41844382570).
- 부팅 45: Mali 레지스터를 한 페이지만 매핑해 AS0(0x2400) 쓰기에서 data abort → 0x5000 전체 매핑.
- 부팅 46 (`xc5gpu=3`, CI run 41846555789): **GPU 첫 작업 성공.** 코어 전원(shader/tiler/L2 1/1/1), WRITE_VALUE를
  identity 모드와 AArch64 4K 페이지 테이블 경유로 각각 실행, `irq=2 js=0 fault=0 word=c0ffee01`, `step=4`.
- 부팅 47 (`xc5gpu=4`, XC5GPU 서비스, CI run 41849540688): `state=1`(서비스 등록), 자체 시험은 `js_status 0x58`
  (DATA_INVALID_FAULT, MMU 결함 없음) → GPU가 0으로 된 작업 헤더를 읽음. kernel_task 매핑이 캐시 가능일 수 있어
  쓰기 후 PoC까지 캐시 정리, 실패 시 2초 대기하지 않게 수정(CI run 42003030139).
- 부팅 48 (CI run 42003030139, 영상 20261105_183953): **XC5GPU 서비스 자체 시험 성공** —
  `gpusvc state=1 bos=1 jobs=1 failed=0 selftest=1 (115 us)`. 사용자 경로(버퍼 생성, GPU 매핑, Submit)로 WRITE_VALUE 실행.
  GPU 3단계 완료. 다음: 4단계(Mesa panfrost iOS 빌드, herodrm을 XC5GPU에 연결, 파일 전달 경로).
- GPU 4단계 준비: Mesa 25.0.7 panfrost iOS 빌드 성공(mesa-ios run 42005058206), CACHE drop zone과 backboardd 삽입
  커널(CI run 42005478588). Odin이 raw cache.img를 거부("Complete(Write) operation failed") → sparse 형식으로 변경.
- 부팅 49 (`xc5-ios-gpu4-console.tar`): **루트 HFS 손상** — `hfs_swap_HFSPlusBTInternalNode: unrecognized catalog record
  type`, `Runtime corruption detected on iOSRoot`, 이어서 rapportd를 쓰레기로 읽어 `non-platform binary` 패닉.
  원인 미확정. 의심: drop zone이 16 MB를 쓰면서 처음 쓰인 512섹터 쓰기 명령(전에는 128). 쓰기는 128로 되돌리고
  DMA 읽기는 처음 64번 뒤에도 256번에 1번 PIO로 비교(CI run 42007309883). USERDATA 재플래시 필요.
- 부팅 50 (USERDATA 재플래시 후, `xc5-ios-gpu4-console.tar` 19:08): XC5GPU selftest=1(125 us). SpringBoard까지 뜬 뒤
  driverkitd/remoted sig 11, `unexpected exec of non-platform binary: /usr/sbin/bluetoothd` 패닉 → 쓰기를 128섹터로
  되돌려도 루트 파일이 틀리게 읽힘. DMA 읽기를 1순위 의심(완료 판정이 버스 쓰기 완료보다 먼저일 수 있음).
  DMA는 기본 끔(`xc5dma=1`로 켬), CI run 42014278955.
- 부팅 51 (USERDATA 재플래시, DMA 끔, `xc5-ios-gpu4-console.tar` 20:00, 영상 20261105_202313): **손상 없음** — 잠금화면,
  패닉 없음(PosterBoard sig 5 한 번). `drop state=3 written=5 failed=0`, `bbd launches=1 inserted=1`. 그러나
  `gpusvc clients=0`: dyld가 DYLD_INSERT_LIBRARIES를 무시(AMFI 정책 응답 없음 → 제한된 프로세스로 취급).
  herolte 0246처럼 "AMFI" MAC 정책이 삽입받은 backboardd pid에만 dyld 플래그를 응답(CI run 42019100873).
- 부팅 52 (CI run 42019100873): **Mesa가 backboardd 안에서 실행됨** — `XC5GPU client: osmesa-test: start`,
  `backboardd sel 5/0/1`, `texture features 0xf7fe007e: ASTC hidden`. 첫 CreateBO에서
  `phystokv_range: illegal PA 0xc0e0f000`(XNU RAM 밖) 패닉. CreateBO 단계별 출력 추가(CI run 42021135234).
- 부팅 53: CreateBO 32 MiB(타일러 힙) → GPU VA 0x1010000, 4 KiB → PA 0x8ab167000, GPU VA 0x3010000 모두 성공. 이어진 MapBO
  (쓰기 결합 매핑)에서 `phystokv_range: illegal PA 0xab167000` → 상위 비트가 잘림. XNU 버그:
  `pmap_sync_page_attributes_phys()`가 32비트 페이지 번호를 넓히기 전에 시프트(`pp << PAGE_SHIFT`). 애플 기기는
  RAM이 4 GiB 아래라 드러나지 않음. `ptoa(pp)`로 수정(CI run 42022755476).

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
