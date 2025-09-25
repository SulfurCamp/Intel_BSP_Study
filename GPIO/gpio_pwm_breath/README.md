# GPIO PWM Breath Module

## 요구 기능
- 모듈 로드 직후 GPIO17과 GPIO19 LED가 1초 주기로 교대로 점등.
- GPIO27 버튼을 누르면 GPIO17/19 토글이 멈추고, GPIO18 LED가 소프트웨어 PWM으로 breathing 효과.
- 버튼을 다시 누르면 PWM이 멈추고 교대 점등이 재개.

## 하드웨어 준비 체크
1. `raspi-gpio get 17 18 19 27` 또는 `gpioinfo gpiochip0 | grep -E '17|18|19|27'`로 핀 상태 확인.
2. 하드웨어 PWM overlay는 필수 아님. 소프트웨어 PWM으로 동작하지만, ALT 기능이 아닌 일반 GPIO로 설정돼 있어야 함.

## 빌드
```bash
cd ~/Intel_study/Intel_BSP/GPIO/gpio_pwm_breath
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
```

## Raspberry Pi에서 실행
```bash
scp gpio_pwm_breath.ko pi@<pi-ip>:/home/pi/intel_study/Intel_BSP/
ssh pi@<pi-ip>
cd ~/intel_study/Intel_BSP
sudo insmod ./gpio_pwm_breath.ko      # 필요 시 gpio 번호 파라미터, chip_label 지정
dmesg | tail
# 동작 확인 후
sudo rmmod gpio_pwm_breath
```

## 모듈 주요 파라미터
| 파라미터 | 기본값 | 설명 |
| --- | --- | --- |
| `led1_gpio` | 17 | 교대 LED 1 |
| `led2_gpio` | 19 | 교대 LED 2 |
| `pwm_gpio` | 18 | breathing LED GPIO |
| `button_gpio` | 27 | 버튼 GPIO |
| `toggle_period_ms` | 1000 | LED 교대 주기 |
| `pwm_period_ns` | 20,000,000 | 소프트웨어 PWM 전체 주기 |
| `pwm_resolution` | 100 | PWM 단계 수 |
| `pwm_step` | 2 | 밝기 변경 단계 |
| `pwm_step_time_ms` | 40 | 밝기 업데이트 주기 |
| `chip_label` | `pinctrl-bcm2711` | GPIO 칩 라벨 |

## 동작 시퀀스
1. 모듈 로드 시 LED17 ON/LED19 OFF 상태에서 시작하고 토글 타이머만 동작.
2. LED 17 ↔ LED 19가 `toggle_period_ms` 간격으로 교대로 켜짐, PWM LED는 꺼져 있음.
3. 버튼을 누르면 토글 타이머를 멈추고 LED17/19를 끈 뒤, PWM LED가 `pwm_step`·`pwm_resolution` 기반 breathing 시작.
4. 버튼을 다시 누르면 PWM 타이머를 멈추고 LED18을 끈 뒤, LED17/19 토글이 재개.

## BSP 디바이스 드라이버 관점의 학습 포인트
- **GPIO 멀티 컨슈머 사용**: 3개의 GPIO(17, 19, 18)를 동시에 제어하며, 디바이스 트리 없이 `gpiochip_request_own_desc()`로 보드 핀을 직접 할당했습니다.
- **상태 기반 제어 로직**: `MODE_TOGGLE/MODE_PWM` 상태 플래그와 헬퍼 함수를 만들어 버튼 이벤트에 따라 서로 다른 동작을 전환하는 커널 모듈 구조를 익혔습니다.
- **소프트웨어 PWM 구현**: 하드웨어 PWM 대신 `hrtimer` 두 개를 조합해 주기(PWM tick)와 듀티 갱신(breath timer)을 분리, 소프트웨어로 밝기를 조절했습니다.
- **인터럽트 스레드 처리**: `request_threaded_irq()`로 버튼 GPIO 인터럽트를 등록하고, 스레드 핸들러에서 토글 ↔ PWM 모드를 안전하게 전환했습니다.
- **모듈 파라미터 확장**: GPIO 번호, 토글 주기, PWM 해상도/속도 등을 `module_param`으로 노출해 BSP 환경에 맞게 조정 가능한 구조를 실습했습니다.

## 로그 예시
- `gpio_pwm_breath: started (LEDs 17/19, PWM GPIO 18, button 27)`
- 버튼 눌러 PWM 전환: `gpio_pwm_breath: PWM breathing started`
- 버튼 눌러 토글 복귀: `gpio_pwm_breath: toggle animation resumed`
- 모듈 제거: `gpio_pwm_breath: module unloaded`

## 확장 아이디어
- `pwm_step`, `pwm_step_time_ms` 파라미터로 breathing 속도 조절.
- 버튼 길게 눌림/짧게 눌림 구분하여 다른 패턴 실행.
- debugfs/sysfs 인터페이스 추가해 사용자 공간에서 주기·듀티 변경.
