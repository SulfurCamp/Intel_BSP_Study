# Device Tree 학습 가이드

## 1. Device Tree 개념
- **정의**: 하드웨어 구성을 커널에 전달하는 선언형 데이터 구조. ARM 기반 SoC에서 커널이 보드·주변장치를 이해하도록 돕는다.
- **구조**: 노드와 속성으로 이루어진 트리; 각 노드가 하드웨어 블록을 표현하며 `compatible`, `reg`, `interrupts` 등을 포함한다.
- **구성 요소**
  - `.dts`: 사람이 읽을 수 있는 소스
  - `.dtsi`: 공통 설정(include)
  - `.dtb`: 컴파일된 바이너리, 부트로더가 커널에 전달
  - `.dtbo`: overlay. 런타임에 추가 장치를 덧붙일 때 사용

## 2. 작성 절차 요약
1. **기본 DTS 선택**: 예) Raspberry Pi 4 → `bcm2711.dtsi` + `bcm2711-rpi-4-b.dts`
2. **주변장치 노드 추가**: UART/I2C/SPI/PWM 등 필요한 디바이스를 해당 버스 노드 아래에 정의 (`compatible`, `reg`, 핀 제어, 클럭 등).
3. **pinctrl 설정**: GPIO 기능, pull-up/down, 드라이브 강도 등을 pinctrl 서브노드에서 선언.
4. **컴파일**: `dtc -I dts -O dtb` 또는 커널 빌드 시 자동 생성.
5. **배포**: `/boot/` 또는 `overlays/` 폴더에 배치 후 `config.txt` 수정.
6. **검증**: 부팅 후 `/proc/device-tree/`, `dmesg`로 노드 등록 여부 확인.

## 3. Device Node 예시
```dts
&i2c1 {
    status = "okay";

    eeprom@50 {
        compatible = "at24,24c32";
        reg = <0x50>;
        pagesize = <32>;
        size = <0x800>;
    };
};
```
- `&i2c1`: 기존 정의된 I2C 컨트롤러 참조
- `status = "okay";`: 컨트롤러 활성화
- `eeprom@50`: 0x50 주소의 I2C 디바이스

## 4. 주변장치별 가이드라인
### UART
- `compatible`으로 UART 드라이버 지정 (예: `"ns16550a"`).
- `reg`에 MMIO 주소, `clocks`/`clock-frequency` 설정.
- `pinctrl-0`에 TX/RX 핀mux 노드를 연결, `status = "okay"`로 활성화.
- 검증: `dmesg`에서 `ttyAMA*` 등록 확인, `/dev/ttyAMA0` 등 장치 존재 확인.

### I2C
- SoC I2C 노드(`i2c@...`)의 `status`를 `okay`로 변경.
- 자식 노드에 센서나 EEPROM을 추가, `compatible`, `reg`(7-bit 주소) 등을 정의.
- 필요한 경우 `interrupts`, `vdd-supply` 등 전원/IRQ 속성 추가.
- 검증: `/dev/i2c-*` 확인, `i2cdetect`로 장치 검색.

### SPI
- 컨트롤러 노드에서 `num-cs`, `cs-gpios`, `pinctrl` 등을 지정하고 `status = "okay"`.
- 자식 노드에 슬레이브 디바이스(`reg`=chip select 번호)와 `spi-max-frequency`, `mode` 등을 선언.
- 검증: `/dev/spidev*` 생성 여부 및 `dmesg`에서 드라이버 로드 확인.

### PWM
- PWM 컨트롤러 노드를 활성화하고 핀mux를 PWM 기능(ALT)으로 설정.
- 소비자 노드(LED, Fan 등)에서 `pwms = <&pwm0 0 20000000 0>;` 형식으로 참조.
- 검증: `/sys/class/pwm/pwmchipX`에서 채널 export 후 동작 확인.

## 5. Overlay 사용 흐름
1. `.dtbo` 빌드 후 `/boot/overlays/`에 배치
2. `/boot/config.txt`에 `dtoverlay=<name>` 추가
3. 재부팅 또는 `dtoverlay` 명령으로 동적 적용
4. `/proc/device-tree/` 및 드라이버 로그 확인

## 6. 커널 모듈과의 연계
- DT에 `compatible`을 정의해두면 커널 모듈의 `of_match_table`로 자동 바인딩.
- 모듈은 `of_get_named_gpio()`, `devm_pwm_get()`, `of_property_read_u32()` 등을 이용해 리소스를 획득.
- 현재 GPIO/PWM 실습 모듈은 DT 없이 동작하지만, 향후 `compatible = "intel,gpio-pwm-breath"` 같은 노드를 추가하면 DT 기반 초기화로 발전 가능.

## 7. 학습 로드맵
1. 기본 DTS 구조 파악 (`bcm2711.dtsi`, 보드별 DTS 읽기)
2. 간단한 overlay 작성해 GPIO LED 노드를 선언하고 `/sys/class/leds` 확인
3. I2C/SPI 디바이스 노드를 추가해 드라이버 자동 로드 경험
4. PWM/Timer 리소스를 선언해 하드웨어 PWM 실습
5. DT와 연동되는 커널 모듈(`of_match_table`) 작성

## 8. 참고 자료
- Linux Kernel `Documentation/devicetree/`
- Devicetree.org 스펙 문서
- Raspberry Pi `config.txt` 문서
- `dtc(1)` 매뉴얼

## 9. 예제: 간단한 LED Overlay
`simple-led-overlay.dts`는 GPIO17을 `gpio-leds` 드라이버에 연결하는 기본 Overlay 예시입니다.

```dts
/dts-v1/;
/plugin/;

/ {
    compatible = "raspberrypi,4-model-b";

    fragment@0 {
        target = <&gpio>;
        __overlay__ {
            simple_led_pins: simple_led_pins {
                brcm,pins = <17>;
                brcm,function = <1>; /* output */
                brcm,pull = <0>;     /* none */
            };
        };
    }

    fragment@1 {
        target-path = "/";
        __overlay__ {
            simple_leds: simple_leds {
                compatible = "gpio-leds";

                simple_led {
                    label = "dt-simple-led";
                    gpios = <&gpio 17 0>;
                    default-state = "off";
                    linux,default-trigger = "heartbeat";
                    pinctrl-names = "default";
                    pinctrl-0 = <&simple_led_pins>;
                };
            };
        }
    }
};
```
- fragment@0: GPIO17을 출력 모드로 pinctrl 설정.
- fragment@1: `gpio-leds` 노드를 추가해 커널 LED 프레임워크가 GPIO17을 LED 장치로 등록.
- `.dtbo`로 컴파일 후 `dtoverlay=simple-led-overlay`를 `config.txt`에 추가하면 `/sys/class/leds/dt-simple-led/`가 생성됨.

---
Device Tree는 단순 GPIO 제어를 넘어 BSP 전체 하드웨어를 선언하는 핵심 도구입니다. 실습 시 기존 DTS를 백업하고, 부팅 실패에 대비한 복구 수단(UART 콘솔, 예비 SD 카드 등)을 준비하세요.
