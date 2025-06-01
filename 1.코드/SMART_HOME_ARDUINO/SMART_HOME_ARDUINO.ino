/* WiFiEsp test: ClientTest
http://www.kccistc.net/
작성일 : 2025.02.27
작성자 : IoT 임베디드 KSH
수정일 : 2025.03.04
수정내용 : MOTOR@REFRESH 명령어로 역방향 모터 제어 추가
수정일 : 2025.03.05
수정내용 : 온도/습도/불쾌지수 체크 및 부저 알림 기능 추가
수정일 : 2025.03.06
수정내용 : MODE@AUTO/MANUAL 명령어 처리 개선 및 환경 센서 연동 강화
*/

#define DEBUG
#include <WiFiEsp.h>
#include <SoftwareSerial.h>
#include <MsTimer2.h>
#include <DHT.h>  // DHT 센서 라이브러리 추가

#define AP_SSID "embA"
#define AP_PASS "embA1234"
#define SERVER_NAME "10.10.141.73"
#define SERVER_PORT 5000
#define LOGID "LJB_ARD"

// 핀 정의
#define BUTTON_PIN 2
#define LED_LAMP_PIN 3
#define WIFIRX 8
#define WIFITX 7
#define MOTOR_PIN 6       // PWM 핀 (모터 속도 제어)
#define MOTOR_DIR1_PIN 5  // 모터 방향 제어 1
#define MOTOR_DIR2_PIN 4  // 모터 방향 제어 2
#define LED_BUILTIN_PIN 13
#define BUZZER_PIN 9    // 부저 핀 정의
#define DHT_PIN 10      // DHT 센서 핀 정의
#define DHT_TYPE DHT22  // DHT 센서 타입 정의 (DHT22 또는 DHT11)

#define CMD_SIZE 50
#define ARR_CNT 5

// 선풍기 모드 정의
#define FAN_OFF 0
#define FAN_LOW 1
#define FAN_MEDIUM 2
#define FAN_HIGH 3
#define FAN_REFRESH 4  // 리프레시 모드 추가 - 역방향 모드

// 모터 방향 정의
#define DIRECTION_FORWARD 0
#define DIRECTION_REVERSE 1

// 환경 임계값 정의
#define TEMP_THRESHOLD 30.0        // 온도 임계값 (섭씨)
#define HUMID_THRESHOLD 70.0       // 습도 임계값 (%)
#define DISCOMFORT_THRESHOLD 80.0  // 불쾌지수 임계값

bool timerIsrFlag = false;
boolean lastButton = LOW;
boolean currentButton = LOW;
boolean ledOn = false;
boolean motorOn = false;                      // 모터 상태를 저장하는 변수
boolean wifiConnected = false;                // WiFi 연결 상태 추적
boolean buzzerActive = false;                 // 부저 활성화 상태
boolean manualMode = true;                    // 수동 모드 상태 추적 (기본값을 true로 변경)
int buttonPressCount = 0;                     // 버튼 누름 횟수 카운트
int fanMode = FAN_OFF;                        // 선풍기 모드 (0: 꺼짐, 1: 약, 2: 중, 3: 강, 4: 리프레시)
int motorDirection = DIRECTION_FORWARD;       // 모터 방향 (0: 정방향, 1: 역방향)
unsigned long lastBuzzerToggleTime = 0;       // 부저 토글 시간 추적
unsigned long lastSensorReadTime = 0;         // 센서 읽기 시간 추적
unsigned long motorStartTime = 0;             // 모터 작동 시작 시간
boolean motorTimeoutWarning = false;          // 모터 작동 타임아웃 경고 상태
const unsigned long MOTOR_TIMEOUT = 3600000;  // 모터 작동 타임아웃 (1시간 = 3600000ms)

// 환경 데이터 저장 변수
float temperature = 0;
float humidity = 0;
float discomfortIndex = 0;

char sendBuf[CMD_SIZE];
unsigned int secCount;

SoftwareSerial wifiSerial(WIFIRX, WIFITX);
WiFiEspClient client;
DHT dht(DHT_PIN, DHT_TYPE);

void setup() {
#ifdef DEBUG
  Serial.begin(38400);
  // Serial.println("아두이노 시작 - 선풍기 모드 (환경 감지 기능 추가)");
#endif

  // 핀 모드 설정
  pinMode(BUTTON_PIN, INPUT_PULLUP);  // 풀업 저항 사용
  pinMode(LED_LAMP_PIN, OUTPUT);
  pinMode(LED_BUILTIN_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);  // 부저 핀 설정

  // 부저 초기화 (꺼진 상태로)
  digitalWrite(BUZZER_PIN, LOW);

  // 모터 핀 설정 (명확하게 정의)
  pinMode(MOTOR_PIN, OUTPUT);
  pinMode(MOTOR_DIR1_PIN, OUTPUT);
  pinMode(MOTOR_DIR2_PIN, OUTPUT);

  // 모터 초기화 (정지 상태로)
  digitalWrite(MOTOR_DIR1_PIN, LOW);
  digitalWrite(MOTOR_DIR2_PIN, LOW);
  analogWrite(MOTOR_PIN, 0);

  // DHT 센서 초기화
  dht.begin();

#ifdef DEBUG
// Serial.println("모터 핀 초기화 완료");
// Serial.println("DHT 센서 초기화 완료");
// Serial.println("선풍기 모드: 꺼짐");
// Serial.println("기본 모드: MANUAL");
#endif

  // LED 초기화 (꺼진 상태로)
  digitalWrite(LED_LAMP_PIN, LOW);
  digitalWrite(LED_BUILTIN_PIN, LOW);

  // 타이머 설정
  MsTimer2::set(1000, timerIsr);
  MsTimer2::start();

  // WiFi 설정 (실패해도 계속 진행)
  setupWifi();

  // 초기화 완료 표시 (LED 깜빡임)
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_BUILTIN_PIN, HIGH);
    delay(100);
    digitalWrite(LED_BUILTIN_PIN, LOW);
    delay(100);
  }

// 모터 작동 테스트 (초기화 시 짧게 작동)
#ifdef DEBUG
// Serial.println("모터 작동 테스트 시작");
#endif

  testMotor();

#ifdef DEBUG
// Serial.println("초기화 및 테스트 완료");
// Serial.println("버튼을 눌러 선풍기 모드를 조절하세요");
#endif

  // 초기화 직후 현재 모드를 서버에 알림
  if (wifiConnected && client.connected()) {
    char msg_to_send[CMD_SIZE];
    sprintf(msg_to_send, "[%s]MODE_STATUS@MANUAL\n", LOGID);
    client.write(msg_to_send, strlen(msg_to_send));

#ifdef DEBUG
// Serial.println("서버에 초기 모드 상태 전송: MANUAL");
#endif
  }
}

// 모터 테스트 함수 (초기화 시 작동 확인)
void testMotor() {
  // 모터 방향 설정 (정방향)
  digitalWrite(MOTOR_DIR1_PIN, HIGH);
  digitalWrite(MOTOR_DIR2_PIN, LOW);

  // 모터 작동 (중간 속도)
  analogWrite(MOTOR_PIN, 150);

#ifdef DEBUG
// Serial.println("모터 테스트: 작동 중");
#endif

  delay(500);  // 0.5초 동안 작동

  // 모터 정지
  analogWrite(MOTOR_PIN, 0);
  digitalWrite(MOTOR_DIR1_PIN, LOW);
  digitalWrite(MOTOR_DIR2_PIN, LOW);

#ifdef DEBUG
// Serial.println("모터 테스트: 정지");
#endif
}

void loop() {
  // 버튼 상태 확인
  currentButton = debounce(lastButton);

  // 버튼 상태 변화 감지 (INPUT_PULLUP 사용으로 LOW가 눌린 상태)
  if (lastButton == HIGH && currentButton == LOW) {
    // 버튼 누름 횟수 증가
    buttonPressCount++;
    // 리프레시 모드 제거로 4개 모드로 변경 (0, 1, 2, 3)
    buttonPressCount = (buttonPressCount % 4);

    // 버튼 누름 횟수에 따라 모드 설정
    switch (buttonPressCount) {
      case 1:  // 약풍 모드 (30%)
        setFanMode(FAN_LOW);
        turnOnLED();
        break;

      case 2:  // 중풍 모드 (60%)
        setFanMode(FAN_MEDIUM);
        // LED 상태 유지
        break;

      case 3:  // 강풍 모드 (100%)
        setFanMode(FAN_HIGH);
        // LED 상태 유지
        break;

      case 0:  // 전원 끄기
        setFanMode(FAN_OFF);
        turnOffLED();
        break;
    }

    // 서버 연결 상태인 경우에만 메시지 전송
    if (wifiConnected && client.connected()) {
      // 버튼 누르면 항상 MANUAL 모드로 변경
      if (!manualMode) {
        // 모드 변경 명령 전송
        char mode_msg[CMD_SIZE];
        sprintf(mode_msg, "[%s]MODE@MANUAL\n", LOGID);
        client.write(mode_msg, strlen(mode_msg));
        manualMode = true;

#ifdef DEBUG
        Serial.println("모드 상태 변경: MANUAL");
#endif

        delay(100);  // 명령 처리 시간 확보
      }

      // 모터 제어 명령 전송
      int speed = (fanMode == FAN_OFF) ? 0 : fanMode * 30;
      char motor_msg[CMD_SIZE];
      sprintf(motor_msg, "[%s]MOTOR@%d\n", LOGID, speed);
      client.write(motor_msg, strlen(motor_msg));
    }
  }

  // 현재 모터 상태에 따라 모터 동작 유지 (중요)
  maintainMotorState();

  // 이전 버튼 상태 저장
  lastButton = currentButton;

  // WiFi/서버 관련 처리
  if (client.available()) {
    socketEvent();
  }

  // 타이머 이벤트 처리
  if (timerIsrFlag) {
    timerIsrFlag = false;

    // 5초마다 센서 체크 (MANUAL 모드에서도 작동)
    if (secCount % 5 == 0) {
      if (fanMode == FAN_OFF) {  // 모터가 꺼져있을 때만 체크
        checkSensors();
      }
    }

    // MANUAL 모드에서 모터 타임아웃 체크 (추가된 부분)
    if (manualMode && motorOn) {
      checkMotorTimeout();
    }

    // 부저 상태 업데이트 (토글)
    if (buzzerActive) {
      updateBuzzer();
    }

    // 30초마다 연결 상태 확인 및 필요시 재연결
    if (secCount % 30 == 0) {
      checkConnection();

      // 현재 모드 상태 전송 (정기적 업데이트)
      if (wifiConnected && client.connected()) {
        char status_msg[CMD_SIZE];
        sprintf(status_msg, "[%s]MODE_STATUS@%s\n", LOGID, manualMode ? "MANUAL" : "AUTO");
        client.write(status_msg, strlen(status_msg));

#ifdef DEBUG
// Serial.print("정기 모드 상태 전송: ");
// Serial.println(manualMode ? "MANUAL" : "AUTO");
#endif
      }
    }

    // 센서 읽기 (10초마다)
    if (secCount % 10 == 0) {
      readSensors();

#ifdef DEBUG
// Serial.print("온도: ");
// Serial.print(temperature);
// Serial.print("°C, 습도: ");
// Serial.print(humidity);
// Serial.print("%, 불쾌지수: ");
// Serial.println(discomfortIndex);
// Serial.print("동작 모드: ");
// Serial.println(manualMode ? "MANUAL" : "AUTO");
#endif
    }
  }
}

// 센서값 읽기 함수
void readSensors() {
  float newTemp = dht.readTemperature();
  float newHumid = dht.readHumidity();

  // 유효한 값인지 확인
  if (!isnan(newTemp) && !isnan(newHumid)) {
    temperature = newTemp;
    humidity = newHumid;

    // 불쾌지수 계산 (온도와 습도 사용)
    // 공식: DI = 0.81 × T + 0.01 × H × (0.99 × T − 14.3) + 46.3
    discomfortIndex = 0.81 * temperature + 0.01 * humidity * (0.99 * temperature - 14.3) + 46.3;
  } else {
#ifdef DEBUG
// Serial.println("센서 읽기 오류: 유효하지 않은 값");
#endif
  }
}

// 센서 조건 확인 및 부저 제어 함수
void checkSensors() {
  // 먼저 센서 데이터 업데이트
  readSensors();

  // 임계값 확인
  bool tempExceeded = temperature >= TEMP_THRESHOLD;
  bool humidExceeded = humidity >= HUMID_THRESHOLD;
  bool discomfortExceeded = discomfortIndex >= DISCOMFORT_THRESHOLD;

  // 어느 하나라도 임계값 초과시 부저 활성화
  if (tempExceeded || humidExceeded || discomfortExceeded) {
    if (!buzzerActive) {
#ifdef DEBUG
// Serial.println("환경 조건 임계값 초과! 부저 활성화");
// if (tempExceeded) Serial.println("온도 임계값 초과");
// if (humidExceeded) Serial.println("습도 임계값 초과");
// if (discomfortExceeded) Serial.println("불쾌지수 임계값 초과");
#endif

      activateBuzzer();
    }
  } else {
    if (buzzerActive) {
      deactivateBuzzer();
    }
  }
}

// 부저 활성화 함수
void activateBuzzer() {
  buzzerActive = true;
  digitalWrite(BUZZER_PIN, HIGH);
  lastBuzzerToggleTime = millis();
}

// 부저 비활성화 함수
void deactivateBuzzer() {
  buzzerActive = false;
  digitalWrite(BUZZER_PIN, LOW);
}

// 부저 토글 업데이트 (경고음 패턴 생성)
void updateBuzzer() {
  // 300ms마다 부저 상태 토글 (경고음 패턴)
  unsigned long currentTime = millis();
  if (currentTime - lastBuzzerToggleTime >= 300) {
    lastBuzzerToggleTime = currentTime;

    // 부저 상태 반전
    if (digitalRead(BUZZER_PIN) == HIGH) {
      digitalWrite(BUZZER_PIN, LOW);
    } else {
      digitalWrite(BUZZER_PIN, HIGH);
    }
  }
}

// 주기적으로 연결 상태 확인 및 재연결 시도 함수
void checkConnection() {
  if (!wifiConnected || !client.connected()) {
    // 이미 연결된 경우 연결 종료
    if (client.connected()) {
      client.stop();
    }

    // WiFi 연결 및 서버 연결 재시도
    setupWifi();
  }
}

// 모터 방향 설정 함수
void setMotorDirection(int direction) {
  motorDirection = direction;

  if (direction == DIRECTION_FORWARD) {
    digitalWrite(MOTOR_DIR1_PIN, HIGH);
    digitalWrite(MOTOR_DIR2_PIN, LOW);
#ifdef DEBUG
    // Serial.println("모터 방향: 정방향");
#endif
  } else {
    digitalWrite(MOTOR_DIR1_PIN, LOW);
    digitalWrite(MOTOR_DIR2_PIN, HIGH);
#ifdef DEBUG
    // Serial.println("모터 방향: 역방향");
#endif
  }
}

// 선풍기 모드 설정 함수
void setFanMode(int mode) {
  fanMode = mode;

  switch (mode) {
    case FAN_OFF:  // 꺼짐
      motorOn = false;
      stopMotor();
      // 모터가 꺼질 때 타임아웃 경고 해제
      motorTimeoutWarning = false;
      // 모터가 꺼질 때 환경 센서 체크 (MANUAL 모드에서만)
      if (manualMode) {
        checkSensors();
      }
      // 부저가 타임아웃으로 활성화된 경우 끄기
      if (buzzerActive && motorTimeoutWarning) {
        deactivateBuzzer();
      }
      break;

    case FAN_LOW:  // 약풍 (30%)
      motorOn = true;
      setMotorDirection(DIRECTION_FORWARD);  // 정방향 설정
      setMotorSpeed(30);
      // 모터가 켜지면 현재 시간 기록
      motorStartTime = millis();
      motorTimeoutWarning = false;  // 경고 상태 초기화
      // 선풍기가 켜지면 부저 끄기
      if (buzzerActive) {
        deactivateBuzzer();
      }
      break;

    case FAN_MEDIUM:  // 중풍 (60%)
      motorOn = true;
      setMotorDirection(DIRECTION_FORWARD);  // 정방향 설정
      setMotorSpeed(60);
      // 모터가 켜지면 현재 시간 기록
      motorStartTime = millis();
      motorTimeoutWarning = false;  // 경고 상태 초기화
      // 선풍기가 켜지면 부저 끄기
      if (buzzerActive) {
        deactivateBuzzer();
      }
      break;

    case FAN_HIGH:  // 강풍 (100%)
      motorOn = true;
      setMotorDirection(DIRECTION_FORWARD);  // 정방향 설정
      setMotorSpeed(100);
      // 모터가 켜지면 현재 시간 기록
      motorStartTime = millis();
      motorTimeoutWarning = false;  // 경고 상태 초기화
      // 선풍기가 켜지면 부저 끄기
      if (buzzerActive) {
        deactivateBuzzer();
      }
      break;

    case FAN_REFRESH:  // 리프레시 모드 (역방향)
      motorOn = true;
      setMotorDirection(DIRECTION_REVERSE);  // 역방향 설정
      setMotorSpeed(80);                     // 80% 속도로 작동
      // 모터가 켜지면 현재 시간 기록
      motorStartTime = millis();
      motorTimeoutWarning = false;  // 경고 상태 초기화
      // 선풍기가 켜지면 부저 끄기
      if (buzzerActive) {
        deactivateBuzzer();
      }
#ifdef DEBUG
      // Serial.println("리프레시 모드 활성화 (역방향)");
#endif
      break;
  }
}

// LED 켜기 함수
void turnOnLED() {
  ledOn = true;
  digitalWrite(LED_BUILTIN_PIN, HIGH);
  digitalWrite(LED_LAMP_PIN, HIGH);
}

// LED 끄기 함수
void turnOffLED() {
  ledOn = false;
  digitalWrite(LED_BUILTIN_PIN, LOW);
  digitalWrite(LED_LAMP_PIN, LOW);
}

// 모터 상태 유지 함수 (이 함수가 중요!)
void maintainMotorState() {
  if (motorOn) {
    // 모터가 켜진 상태라면 현재 모드에 맞는 속도 유지
    switch (fanMode) {
      case FAN_LOW:  // 약풍 (30%)
        setMotorSpeed(30);
        break;

      case FAN_MEDIUM:  // 중풍 (60%)
        setMotorSpeed(60);
        break;

      case FAN_HIGH:  // 강풍 (100%)
        setMotorSpeed(100);
        break;

      case FAN_REFRESH:  // 리프레시 모드 (역방향)
        // 역방향 유지
        setMotorDirection(DIRECTION_REVERSE);
        setMotorSpeed(80);
        break;
    }
  } else {
    // 모터가 꺼진 상태라면 정지 유지
    stopMotor();
  }
}

// 모터 정지
void stopMotor() {
  // 속도를 0으로 설정
  analogWrite(MOTOR_PIN, 0);

  // 방향 핀도 LOW로 설정
  digitalWrite(MOTOR_DIR1_PIN, LOW);
  digitalWrite(MOTOR_DIR2_PIN, LOW);
}

// WiFi 초기화 함수
void setupWifi() {
  wifiSerial.begin(38400);

  // WiFi 모듈 초기화
  WiFi.init(&wifiSerial);

  if (WiFi.status() == WL_NO_SHIELD) {
    wifiConnected = false;
    return;
  }

  // WiFi 연결
  wifi_Init();

  // 서버 연결
  if (wifiConnected) {
    server_Connect();
  }
}

void socketEvent() {
  char recvBuf[CMD_SIZE] = { 0 };
  int len = client.readBytesUntil('\n', recvBuf, CMD_SIZE);
  client.flush();

// 원본 메시지 및 길이 출력 (디버깅용)
#ifdef DEBUG
// Serial.print("서버로부터 수신 (길이: ");
// Serial.print(len);
// Serial.print("): '");
// Serial.print(recvBuf);
// Serial.println("'");
#endif

  // 중복 메시지 처리 방지를 위한 정적 변수
  static unsigned long lastCommandTime = 0;
  static char lastCommand[CMD_SIZE] = { 0 };

  // 같은 명령이 짧은 시간 내에 반복되는지 확인 (500ms 이내)
  unsigned long currentTime = millis();
  if (strcmp(recvBuf, lastCommand) == 0 && (currentTime - lastCommandTime) < 500) {
    return;
  }

  // 마지막 명령 업데이트
  strncpy(lastCommand, recvBuf, CMD_SIZE);
  lastCommandTime = currentTime;

  // MODE 명령어 처리 - 명시적으로 AUTO/MANUAL 모드 변경
  // 직접 문자열 검색을 통한 명령 인식 개선
  if (strstr(recvBuf, "MODE@MANUAL") != NULL) {
#ifdef DEBUG
// Serial.println("MODE@MANUAL 명령 직접 감지");
#endif

    manualMode = true;

    // 모드 변경 확인 응답 전송
    sprintf(sendBuf, "[%s]MODE_STATUS@MANUAL\n", LOGID);
    client.write(sendBuf, strlen(sendBuf));

#ifdef DEBUG
// Serial.println("모드 변경: MANUAL");
#endif
  } else if (strstr(recvBuf, "MODE@AUTO") != NULL) {
#ifdef DEBUG
// Serial.println("MODE@AUTO 명령 직접 감지");
#endif

    // 모터가 동작 중인 경우 AUTO 모드로 변경하지 않음
    if (fanMode == FAN_OFF) {
      manualMode = false;

      // 모드 변경 확인 응답 전송
      sprintf(sendBuf, "[%s]MODE_STATUS@AUTO\n", LOGID);
      client.write(sendBuf, strlen(sendBuf));

#ifdef DEBUG
// Serial.println("모드 변경: AUTO");
#endif

      // Auto 모드로 변경 시 부저 끄기
      if (buzzerActive) {
        deactivateBuzzer();
      }
    } else {
// 모터가 동작 중일 때는 모드 변경 거부
#ifdef DEBUG
// Serial.println("모터 동작 중에는 AUTO 모드로 변경할 수 없음");
#endif

      // 모드 변경 거부 응답 전송
      sprintf(sendBuf, "[%s]MODE_STATUS@MANUAL\n", LOGID);
      client.write(sendBuf, strlen(sendBuf));
    }
  }

  // MODE_CHECK 명령 처리 - 현재 모드 상태 응답
  if (strstr(recvBuf, "MODE_CHECK") != NULL) {
#ifdef DEBUG
// Serial.println("MODE_CHECK 명령 직접 감지");
#endif

    // 현재 모드 상태 전송
    sprintf(sendBuf, "[%s]MODE_STATUS@%s\n", LOGID, manualMode ? "MANUAL" : "AUTO");
    client.write(sendBuf, strlen(sendBuf));

#ifdef DEBUG
// Serial.print("모드 확인 요청에 응답: ");
// Serial.println(manualMode ? "MANUAL" : "AUTO");
#endif

    return;
  }

  // MOTOR 명령어 처리 - 직접 문자열 검색 방식으로 개선
  if (strstr(recvBuf, "MOTOR@") != NULL) {
#ifdef DEBUG
    // Serial.print("MOTOR@ 명령 직접 감지: ");
    // Serial.println(recvBuf);
#endif

    // ON 명령 처리 - 100% 속도로 설정 (추가된 부분)
    if (strstr(recvBuf, "MOTOR@ON") != NULL) {
#ifdef DEBUG
      // Serial.println("MOTOR@ON 명령 감지 - 100% 속도로 설정");
#endif

      // 항상 AUTO 모드로 설정 (AUTO 모드에서 자동 제어용)
      manualMode = false;

      setFanMode(FAN_HIGH);
      turnOnLED();
      buttonPressCount = 3;

      // 응답 전송
      sprintf(sendBuf, "[%s]MOTOR@ON\n", LOGID);
      client.write(sendBuf, strlen(sendBuf));

      // 부저가 활성화된 경우 끄기 (모터가 켜졌으므로)
      if (buzzerActive) {
        deactivateBuzzer();
      }
    }
    // 100 명령도 ON과 동일하게 처리 (추가)
    else if (strstr(recvBuf, "MOTOR@100") != NULL) {
#ifdef DEBUG
      // Serial.println("MOTOR@100 명령 감지 - ON과 동일하게 처리");
#endif

      // 항상 AUTO 모드로 설정 (AUTO 모드에서 자동 제어용)
      manualMode = false;

      setFanMode(FAN_HIGH);
      turnOnLED();
      buttonPressCount = 3;

      // 응답 전송 - MOTOR@ON으로 일관성 유지
      sprintf(sendBuf, "[%s]MOTOR@ON\n", LOGID);
      client.write(sendBuf, strlen(sendBuf));

      // 부저가 활성화된 경우 끄기 (모터가 켜졌으므로)
      if (buzzerActive) {
        deactivateBuzzer();
      }
    }

    // REFRESH 명령 처리 - 역방향으로 설정
    else if (strstr(recvBuf, "MOTOR@REFRESH") != NULL) {
#ifdef DEBUG
      // Serial.println("REFRESH 명령 직접 감지");
#endif

      // 항상 MANUAL 모드로 강제 설정
      manualMode = true;

      setFanMode(FAN_REFRESH);
      turnOnLED();
      buttonPressCount = 4;

      // 응답 전송
      sprintf(sendBuf, "[%s]MOTOR@REFRESH\n", LOGID);
      client.write(sendBuf, strlen(sendBuf));

      // 부저가 활성화된 경우 끄기 (모터가 켜졌으므로)
      if (buzzerActive) {
        deactivateBuzzer();
      }
    }
    // 정지 명령 처리 - OFF 포함 (수정된 부분)
    else if (strstr(recvBuf, "MOTOR@0") != NULL || strstr(recvBuf, "MOTOR@OFF") != NULL) {
#ifdef DEBUG
      // Serial.println("MOTOR@OFF 또는 MOTOR@0 명령 감지");
#endif

      setFanMode(FAN_OFF);
      turnOffLED();
      buttonPressCount = 0;

      // 응답 전송
      sprintf(sendBuf, "[%s]MOTOR@OFF\n", LOGID);
      client.write(sendBuf, strlen(sendBuf));

      // 모터가 꺼졌으므로 환경 센서 체크
      checkSensors();
    }
    // 속도 명령 처리
    else {
      int speed = 0;
      // MOTOR@60 형식에서 속도값 추출
      char *speed_str = strstr(recvBuf, "MOTOR@") + 6;
      speed = atoi(speed_str);

#ifdef DEBUG
      // Serial.print("모터 속도 명령 파싱: ");
      // Serial.println(speed);
#endif

      // 모터 속도가 설정되면 자동으로 MANUAL 모드로 전환
      manualMode = true;

      if (speed <= 40) {
        setFanMode(FAN_LOW);
        turnOnLED();
        buttonPressCount = 1;
      } else if (speed <= 70) {
        setFanMode(FAN_MEDIUM);
        turnOnLED();
        buttonPressCount = 2;
      } else {
        setFanMode(FAN_HIGH);
        turnOnLED();
        buttonPressCount = 3;
      }

      // 응답 전송
      sprintf(sendBuf, "[%s]MOTOR@%d\n", LOGID, speed);
      client.write(sendBuf, strlen(sendBuf));

      // 부저가 활성화된 경우 끄기 (모터가 켜졌으므로)
      if (buzzerActive) {
        deactivateBuzzer();
      }
    }

    return;  // 모터 명령 처리 후 조기 반환
  }

  // 기존 명령어 처리 로직 - 백업용으로 유지
  char *pArray[ARR_CNT] = { 0 };
  char *pToken = strtok(recvBuf, "[@]");
  int i = 0;

  while (pToken != NULL && i < ARR_CNT) {
    pArray[i++] = pToken;
    pToken = strtok(NULL, "[@]");
  }

#ifdef DEBUG
// Serial.print("백업 파싱 토큰 개수: ");
// Serial.println(i);
// if (i > 0) {
//   Serial.print("첫번째 토큰: ");
//   Serial.println(pArray[0]);
// }
// if (i > 1) {
//   Serial.print("두번째 토큰: ");
//   Serial.println(pArray[1]);
// }
// if (i > 2) {
//   Serial.print("세번째 토큰: ");
//   Serial.println(pArray[2]);
// }
#endif

  if (i >= 3) {
    if (!strcmp(pArray[1], "LED")) {
      bool ledState = !strcmp(pArray[2], "ON");
      if (ledState) {
        turnOnLED();
      } else {
        turnOffLED();
      }
    } else if (!strcmp(pArray[1], "FAN")) {
      int mode = atoi(pArray[2]);
      if (mode >= 0 && mode <= 4) {
        setFanMode(mode);
        buttonPressCount = (mode == 0) ? 0 : mode;
        if (mode == 0) {
          turnOffLED();
        } else {
          turnOnLED();
        }
      }
    }
    // 백업 MODE 처리
    else if (!strcmp(pArray[1], "MODE")) {
#ifdef DEBUG
// Serial.print("백업 처리: MODE 명령 감지: ");
// Serial.println(pArray[2]);
#endif

      if (!strcmp(pArray[2], "MANUAL")) {
        manualMode = true;

#ifdef DEBUG
// Serial.println("백업 처리: 모드 변경: MANUAL");
#endif

        // 응답 전송
        sprintf(sendBuf, "[%s]MODE_STATUS@MANUAL\n", LOGID);
        client.write(sendBuf, strlen(sendBuf));
      } else if (!strcmp(pArray[2], "AUTO")) {
        if (fanMode == FAN_OFF) {
          manualMode = false;

#ifdef DEBUG
// Serial.println("백업 처리: 모드 변경: AUTO");
#endif

          // 응답 전송
          sprintf(sendBuf, "[%s]MODE_STATUS@AUTO\n", LOGID);
          client.write(sendBuf, strlen(sendBuf));

          // 부저 끄기
          if (buzzerActive) {
            deactivateBuzzer();
          }
        } else {
#ifdef DEBUG
          Serial.println("백업 처리: 모터 동작 중에는 AUTO 모드로 변경할 수 없음");
#endif

          // 응답 전송
          sprintf(sendBuf, "[%s]MODE_STATUS@MANUAL\n", LOGID);
          client.write(sendBuf, strlen(sendBuf));
        }
      }
    }
    // 백업 MOTOR 처리
    else if (!strcmp(pArray[1], "MOTOR")) {
#ifdef DEBUG
      Serial.print("백업 처리: MOTOR 명령 감지: ");
      Serial.println(pArray[2]);
#endif

      // REFRESH 명령 처리
      if (!strcmp(pArray[2], "REFRESH")) {
#ifdef DEBUG
        Serial.println("백업 처리: REFRESH 명령 감지");
#endif

        // 항상 MANUAL 모드로 설정
        manualMode = true;

        setFanMode(FAN_REFRESH);
        turnOnLED();
        buttonPressCount = 4;

        // 응답 전송
        sprintf(sendBuf, "[%s]MOTOR@REFRESH\n", LOGID);
        client.write(sendBuf, strlen(sendBuf));

        if (buzzerActive) {
          deactivateBuzzer();
        }
      }
      // OFF 명령 처리
      else if (!strcmp(pArray[2], "OFF") || !strcmp(pArray[2], "0")) {
#ifdef DEBUG
        Serial.println("백업 처리: MOTOR OFF 명령 감지");
#endif

        setFanMode(FAN_OFF);
        turnOffLED();
        buttonPressCount = 0;

        // 응답 전송
        sprintf(sendBuf, "[%s]MOTOR@0\n", LOGID);
        client.write(sendBuf, strlen(sendBuf));

        // 환경 센서 체크
        checkSensors();
      }
      // 속도 명령 처리
      else {
        int speed = atoi(pArray[2]);

#ifdef DEBUG
        Serial.print("백업 처리: 모터 속도 설정: ");
        Serial.println(speed);
#endif

        // MANUAL 모드로 설정
        manualMode = true;

        if (speed <= 40) {
          setFanMode(FAN_LOW);
          turnOnLED();
          buttonPressCount = 1;
        } else if (speed <= 70) {
          setFanMode(FAN_MEDIUM);
          turnOnLED();
          buttonPressCount = 2;
        } else {
          setFanMode(FAN_HIGH);
          turnOnLED();
          buttonPressCount = 3;
        }

        // 응답 전송
        sprintf(sendBuf, "[%s]MOTOR@%d\n", LOGID, speed);
        client.write(sendBuf, strlen(sendBuf));

        if (buzzerActive) {
          deactivateBuzzer();
        }
      }
    }
  }
}

void timerIsr() {
  timerIsrFlag = true;
  secCount++;
}

void wifi_Init() {
  // WiFi 연결 시도 (최대 5회로 증가)
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 5) {
    WiFi.begin(AP_SSID, AP_PASS);

    // 연결 대기 (10회 * 500ms = 5초)
    for (int i = 0; i < 10; i++) {
      delay(500);

      if (WiFi.status() == WL_CONNECTED) {
        break;
      }
    }

    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
#ifdef DEBUG
    Serial.println("WiFi 연결 성공");
#endif
  } else {
    wifiConnected = false;
#ifdef DEBUG
    Serial.println("WiFi 연결 실패");
#endif
  }
}

int server_Connect() {
  if (!wifiConnected) {
    return 0;
  }

  // 이미 연결된 경우 연결 종료
  if (client.connected()) {
    client.stop();
    delay(500);
  }

  // 여러 번 연결 시도
  int attempts = 0;
  bool connected = false;

  while (!connected && attempts < 3) {
    if (client.connect(SERVER_NAME, SERVER_PORT)) {
      connected = true;
    } else {
      delay(1000);
      attempts++;
    }
  }

  if (connected) {
    // ID 식별자 전송
    client.print("[" LOGID "]");
#ifdef DEBUG
    Serial.println("서버 연결 성공");
#endif

    // 연결 성공 시 현재 모드 상태 전송
    if (connected) {
      delay(500);  // 서버 연결 안정화 대기

      char mode_msg[CMD_SIZE];
      sprintf(mode_msg, "[%s]MODE_STATUS@%s\n", LOGID, manualMode ? "MANUAL" : "AUTO");
      client.write(mode_msg, strlen(mode_msg));

#ifdef DEBUG
      Serial.print("서버 연결 후 모드 상태 전송: ");
      Serial.println(manualMode ? "MANUAL" : "AUTO");
#endif
    }

    return 1;
  } else {
#ifdef DEBUG
    Serial.println("서버 연결 실패");
#endif
    return 0;
  }
}

void checkMotorTimeout() {
  // MANUAL 모드이고 모터가 켜져 있는 경우에만 체크
  if (manualMode && motorOn && fanMode != FAN_OFF) {
    unsigned long currentTime = millis();

    // 모터 작동 시간이 1시간을 초과하는지 확인
    if (currentTime - motorStartTime >= MOTOR_TIMEOUT && !motorTimeoutWarning) {
      // 1시간 초과 시 부저 경고 활성화
      motorTimeoutWarning = true;
      activateTimeoutBuzzer();

#ifdef DEBUG
      Serial.println("모터 작동 1시간 초과! 부저 경고 활성화");
#endif
    }
  }
}

void activateTimeoutBuzzer() {
  // 기존 buzzerActive가 false일 때만 활성화 (환경 경고와 충돌 방지)
  if (!buzzerActive) {
    buzzerActive = true;
    digitalWrite(BUZZER_PIN, HIGH);
    lastBuzzerToggleTime = millis();

#ifdef DEBUG
    Serial.println("타임아웃 부저 활성화");
#endif
  }
}

void setMotorSpeed(int speedPercent) {
  // 입력 범위 제한 (0-100%)
  if (speedPercent < 0) speedPercent = 0;
  if (speedPercent > 100) speedPercent = 100;

  // 0-100% 범위를 0-255 PWM 값으로 변환
  int pwmValue = map(speedPercent, 0, 100, 0, 255);

  // PWM 값이 너무 작으면 모터가 회전하지 않을 수 있으므로 최소값 설정
  if (speedPercent > 0 && pwmValue < 150) {
    pwmValue = 150;  // 최소 PWM 값 설정
  }

  // 모터 속도 설정
  analogWrite(MOTOR_PIN, pwmValue);

#ifdef DEBUG
// Serial.print("모터 속도 설정: ");
// Serial.print(speedPercent);
// Serial.print("% (PWM: ");
// Serial.print(pwmValue);
// Serial.println(")");
#endif
}

// 개선된 디바운싱 함수
boolean debounce(boolean last) {
  boolean current = digitalRead(BUTTON_PIN);
  if (last != current) {
    delay(10);  // 디바운스 시간
    current = digitalRead(BUTTON_PIN);
  }
  return current;
}