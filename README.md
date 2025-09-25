# Raspberry Pi 4 BSP 크로스 컴파일 정리

## 작업 환경
- Host: Ubuntu (교차 컴파일 환경)
- Target: Raspberry Pi 4 Model B (BCM2711)
- Target 커널 버전: `6.12.48-v7l+ #1 SMP Wed Sep 24 11:08:43 KST 2025 armv7l`
- 레포지토리: [raspberrypi/linux](https://github.com/raspberrypi/linux)

## 진행 순서
1. **대상 보드의 커널 버전 확인**
   - `uname -a`로 Raspberry Pi에서 현재 올라간 커널 버전을 확인.
   - BSP 작업 시에는 *대상 보드의 커널 버전에 맞는* 소스 코드를 기준으로 교차 컴파일해야 함.
2. **커널 소스 준비**
   - Raspberry Pi 전용 커널 레포지토리를 클론 후, 보드에 올라가 있는 커널 버전(예: `rpi-6.12.y`)과 일치하는 브랜치/태그를 체크아웃.
   - 호스트 쪽 커널 버전을 맞추는 것이 아니라, *라즈베리파이에 설치된 커널 버전에 맞는 소스*를 준비하는 것이 핵심.
3. **교차 컴파일 설정**
   - 커널 루트 디렉터리에서 `make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- bcm2711_defconfig` 등 목표 보드용 디폴트 설정을 적용.
   - 필요 시 Menuconfig 등으로 옵션 조정.
4. **커널 이미지, DTB, 모듈 빌드**
   - `make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- zImage`
   - `make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- dtbs`
   - `make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules`
5. **산출물 설치 및 전송**
   - 루트 파일시스템 staging 디렉터리를 잡고 `make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- modules_install INSTALL_MOD_PATH=<rootfs>` 실행해 모듈 설치 파일을 준비.
   - `zImage`, `*.dtb`, `overlays/*.dtbo`, `modules`를 SD카드 또는 네트워크를 통해 Raspberry Pi의 `/boot` 및 `/lib/modules/<kernel-version>/`에 반영.
   - SD카드를 직접 마운트하거나, 라즈베리파이를 미리 부팅해 두었다면 `scp` 등으로 전송 후 백업본과 교체.
6. **모듈 테스트**
   - 샘플 메시지 출력 커널 모듈을 교차 컴파일하거나 라즈베리파이 위에서 직접 빌드.
   - `insmod`, `rmmod`로 모듈 로드/언로드 후 `dmesg`에서 printk 메시지 확인.

## 핵심 체크포인트
- `make`는 항상 커널 최상위 디렉터리에서 실행.
- 커널 버전 불일치가 있으면 DTB, 모듈, zImage가 부팅 중 로드되지 않거나 심볼 충돌 발생.
- 새로 빌드한 커널/DTB/모듈을 실제 타겟에 반영할 때는 기존 파일 백업 필수.
- 교차 컴파일러와 `ARCH`, `CROSS_COMPILE` 변수를 명확하게 지정해 빌드 환경을 통일.
- 빌드 후 `modules.dep` 등이 정확히 설치되었는지 `depmod` 또는 부팅 로그를 통해 검증.

## 정리
- 라즈베리파이4에 이미 올라가 있는 커널 버전과 동일한 브랜치/태그를 선택해 교차 컴파일.
- 커널 이미지, DTB, 모듈을 모두 세트로 빌드하고, 모듈은 `modules_install`로 루트 영역에 설치.
- 산출물을 SD카드를 통해 덮어씌우거나 네트워크 전송 후 교체.
- 간단한 모듈을 insmod/rmmod로 테스트하고 `dmesg`로 동작 확인.

## GPIO17 LED + GPIO27 버튼 예제 모듈

- **소스 경로**: `GPIO/gpio_pwm_irq/gpio_pwm_irq.c`
  - 모듈이 로드되면 GPIO17(LED)을 High로 켜고, 1초 주기의 `hrtimer`로 토글.
  - GPIO27(버튼) 인터럽트가 들어오면 타이머를 멈추고 LED를 Low로 끈 뒤 메시지를 출력.
  - `led_gpio`, `button_gpio`, `period_ms` 세 개의 모듈 파라미터로 핀/주기를 조정 가능.
  - `chip_label` 파라미터로 사용할 GPIO 칩(label)을 지정 가능 (기본값 `pinctrl-bcm2711`).
- **빌드 방법 (호스트 Ubuntu)**
  ```bash
  cd GPIO/gpio_pwm_irq
  make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
  ```
  - Makefile에서 `KDIR` 기본값은 `../..` 위치의 `linux` 커널 트리를 가리키므로 경로가 다르면 `make KDIR=<kernel-src>`로 지정.
  - 산출물: `GPIO/gpio_pwm_irq/gpio_pwm_irq.ko`
- **라즈베리파이 배포/실행**
  ```bash
  scp GPIO/gpio_pwm_irq/gpio_pwm_irq.ko pi@<pi-ip>:/home/pi/
  ssh pi@<pi-ip>
  sudo install -D -m 644 gpio_pwm_irq.ko \
      /lib/modules/$(uname -r)/extra/gpio_pwm_irq.ko
  sudo depmod -a
  sudo insmod /lib/modules/$(uname -r)/extra/gpio_pwm_irq.ko
  # 동작 확인: 1초 간격으로 LED 깜빡임, 버튼 누르면 멈춤
  sudo rmmod gpio_pwm_irq
  ```
- **dmesg 로그 예시**
  - `gpio_pwm_irq: LED blinking started (GPIO17, period 1000ms)`
  - `gpio_pwm_irq: button on GPIO27 to stop blinking`
  - 버튼 누르면 `gpio_pwm_irq: blinking stopped by button`
- **추가 실험 아이디어**
  1. 모듈 파라미터로 `period_ms`를 조정해 속도 변화 확인 (`sudo insmod ... period_ms=250`).
  2. 버튼 인터럽트를 재활성화해 토글 동작(재시작)을 구현하거나, sysfs/debugfs로 상태를 노출.
  3. `hrtimer_forward_now()`를 활용한 소프트웨어 PWM 확장(디밍, breathing LED).

# Intel_BSP 실습 기록 요약

## 현재 진행 중인 BSP 디바이스 드라이버 개념
- **GPIO 컨슈머 API 활용**: 디바이스 트리를 수정하지 않고도 `gpiochip_request_own_desc()`로 전역 GPIO를 안전하게 요청하고, `gpiod_direction_output/input`으로 핀 모드를 제어하는 방법을 연습했습니다.
- **인터럽트 기반 이벤트 처리**: `request_threaded_irq()`를 통해 버튼 GPIO 인터럽트를 등록하고, 스레드 핸들러에서 LED 동작을 제어하여 안전한 IRQ 처리 흐름을 익혔습니다.
- **고해상도 타이머(HRTIMER)**: `hrtimer_init`, `hrtimer_forward_now`로 커널 타이머를 주기적으로 재시작하여 LED 토글, 소프트웨어 PWM 간격 제어를 구현했습니다.
- **소프트웨어 PWM 설계**: 하드웨어 PWM 없이 hrtimer 두 개를 조합해 듀티/주기를 관리하고, breathing LED 효과를 만들어보며 BSP 수준에서의 PWM 제어를 체험했습니다.
- **모듈 파라미터와 상태 머신**: `module_param()`으로 GPIO 번호·주기 등 환경별 설정을 노출하고, 모드 플래그로 토글 ↔ PWM 상태를 전환하는 커널 모듈 구조를 학습했습니다.

## 디렉터리별 실습 정리
- `GPIO/gpio_pwm_irq/README.md`: 단일 LED와 버튼을 이용해 GPIO 제어 + hrtimer + IRQ 연동을 학습한 기록.
- `GPIO/gpio_pwm_breath/README.md`: 두 개의 LED 교대 토글과 소프트웨어 PWM breathing, 버튼 기반 상태 전환을 구현한 기록.
- `BSP.md`: Raspberry Pi 커널 트리와 교차 컴파일 환경 설정, 모듈 빌드/배포 절차 메모.

이 저장소는 Raspberry Pi 4를 대상으로 BSP 디바이스 드라이버의 기초 개념을 실습하며, GPIO, 인터럽트, 타이머, PWM 제어를 단계적으로 확장해 가고 있습니다.
