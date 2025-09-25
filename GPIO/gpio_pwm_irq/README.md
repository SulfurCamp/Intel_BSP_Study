# GPIO17 LED & GPIO27 버튼 모듈 실습 기록

## 목표 및 환경
- Target: Raspberry Pi 4 (BCM2711), 사용자 공간에서 GPIO17 LED와 GPIO27 버튼 연결 확인.
- Host: Ubuntu 교차 컴파일 환경 (`arm-linux-gnueabihf-` 툴체인).
- Kernel tree: `~/Intel_study/Intel_BSP/linux` (라즈베리파이 커널과 동일 브랜치).
- 모듈 소스: `GPIO/gpio_pwm_irq/gpio_pwm_irq.c`.

## 준비 과정
1. **기존 sysfs GPIO 확인**
   - `echo 17 | sudo tee /sys/class/gpio/export` 시 `Invalid argument` → 커널에 `CONFIG_GPIO_SYSFS` 미활성화로 판단.
   - 결론: libgpiod/raspi-gpio 유틸리티 사용으로 방향 변경.
2. **LED 선 연결 검사**
   - `sudo raspi-gpio set 17 op dh` → LED 상시 ON.
   - `sudo raspi-gpio set 17 op dl` → LED OFF.
3. **버튼 동작 확인**
   - `sudo raspi-gpio set 27 ip pu` (내부 풀업) 또는 외부 풀다운 회로 유지.
   - `watch -n 0.2 sudo raspi-gpio get 27` → 누르면 level 변화 확인.
   - Edge 감지 참고: `sudo gpiomon gpiochip0 27`.

## 빌드 & 배포 명령
호스트(Ubuntu)에서:
```bash
cd ~/Intel_study/Intel_BSP/GPIO/gpio_pwm_irq
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
```
- 산출물: `gpio_pwm_irq.ko`
- 실수로 `scp gpio_pwm_irq.ko pi@<ip>`처럼 콜론 없이 실행하면 현재 디렉터리에 `pi@<ip>`라는 파일이 생김 → `scp gpio_pwm_irq.ko pi@<ip>:/home/pi/intel_study/Intel_BSP/`처럼 콜론 포함 필요.

Raspberry Pi에서:
```bash
cd ~/intel_study/Intel_BSP
sudo insmod ./gpio_pwm_irq.ko         # 필요 시 chip_label=<칩라벨>
dmesg | tail
# 테스트 후
sudo rmmod gpio_pwm_irq
```
- 모듈을 `/lib/modules/...`에 상주시킬 계획이면 `sudo install -D`로 복사 후 `sudo depmod -a`, `modprobe gpio_pwm_irq` 권장.

## 커널 모듈 로직 요약 (`GPIO/gpio_pwm_irq/gpio_pwm_irq.c`)
- `module_param`으로 `led_gpio`, `button_gpio`, `period_ms`, `chip_label` 설정 (기본: 17, 27, 1000ms, `pinctrl-bcm2711`).
- `gpio_device_find_by_label()` 실패 시 `gpio_device_find()`로 전체 GPIO 칩 순회 → 주어진 GPIO 번호를 포함하는 칩을 자동 탐지.
- `gpiochip_request_own_desc()`로 LED/버튼 라인을 요청 후 `hrtimer` 시작.
- `hrtimer` 콜백에서 `ms_to_ktime(period_ms)` 주기로 LED 토글.
- `request_threaded_irq()` (with `IRQF_ONESHOT`)을 사용해 버튼 인터럽트를 스레드 핸들러로 처리.
- 버튼을 한 번 누르면 `htimer_cancel()`과 함께 LED 끄기 → 다시 누르면 `atomic_set()` 후 타이머 재시작으로 깜빡임 재개.
- 언로드 시 `gpiochip_free_own_desc()`와 `gpio_device_put()`로 자원 해제.

## BSP 디바이스 드라이버 관점의 학습 포인트
- **GPIO 컨슈머 프레임워크**: 디바이스 트리 없이도 `gpiochip_request_own_desc()`로 전역 GPIO 번호를 안전하게 요청하고, `gpiod_direction_output/input`으로 모드를 지정했습니다.
- **고해상도 타이머(HRTIMER)**: `hrtimer_init`과 `hrtimer_forward_now`를 사용해 커널 타이머가 정밀하게 LED 토글 주기를 유지하도록 구현했습니다.
- **인터럽트 기반 이벤트 처리**: `request_threaded_irq`로 버튼 GPIO의 IRQ를 등록하고, 스레드 컨텍스트에서 LED 상태를 제어해 빠른 토글과 안전한 작업 분리를 경험했습니다.
- **모듈 파라미터 활용**: 핀 번호와 주기를 `module_param`으로 노출해 BSP 설정값을 유연하게 바꿀 수 있는 구조를 연습했습니다.

## 문제 해결 기록
| 이슈 | 현상 | 조치 |
| --- | --- | --- |
| sysfs GPIO 미지원 | `/sys/class/gpio`에서 `Invalid argument` | raspi-gpio/libgpiod로 핀 확인, 커널 모듈은 gpiod API 활용 |
| `gpiod_set_pull` 미정의 | 빌드 오류 (`implicit declaration`) | 해당 호출 제거, 필요 시 사용자 공간에서 풀업 설정 |
| `gpio_to_desc()` 실패 | `failed to translate GPIO 17` | GPIO 칩 라벨 검색 + `gpiochip_request_own_desc()` 사용으로 수정 |
| IRQ 요청 오류 | `Threaded irq requested with handler=NULL and !ONESHOT` | `IRQF_ONESHOT` 플래그 추가 |
| 버튼이 한 번만 동작 | 토글 불가 | 스레드 핸들러에 재시작 로직 추가 (LED 켜고 타이머 재시작) |

## 동작 확인
1. 모듈 로드 직후 LED는 켜진 상태로 시작하며 1초 주기로 점멸.
2. 버튼 입력 시:
   - 첫 번째 눌림 → LED 끄고 점멸 중지 (`dmesg`: `blinking stopped by button`).
   - 두 번째 눌림 → LED 켜지고 점멸 재개 (`dmesg`: `blinking resumed by button`).
3. `sudo rmmod gpio_pwm_irq`로 모듈 제거 시 LED를 끄고 GPIO 리소스 정리 후 메시지 출력.

## 참고 명령 모음
- GPIO 칩 라벨 확인: `gpioinfo` (libgpiod 패키지).
- 핀 상태 빠르게 확인: `raspi-gpio get 17 27`.
- 모듈 로그 확인: `dmesg | tail -n 50`.

## 추가 확장 아이디어
1. `period_ms`를 sysfs/debugfs에 노출해 사용자 공간에서 주기 조절.
2. 듀티 사이클 파라미터 추가 후 software PWM (breathing LED) 실험.
3. 버튼을 길게 누를 때와 짧게 누를 때 다른 동작을 하도록 workqueue 예약.
