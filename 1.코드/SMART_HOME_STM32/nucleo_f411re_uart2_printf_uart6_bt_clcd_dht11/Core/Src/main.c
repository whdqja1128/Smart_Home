/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "clcd.h"
#include "dht.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
// 시간 관리를 위한 구조체 정의
typedef struct {
  int year;
  int month;
  int day;
  int hour;
  int min;
  int sec;
} DATETIME;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#ifdef __GNUC__
/* With GCC, small printf (option LD Linker->Libraries->Small printf
   set to 'Yes') calls __io_putchar() */
#define PUTCHAR_PROTOTYPE int __io_putchar(int ch)
#else
#define PUTCHAR_PROTOTYPE int fputc(int ch, FILE *f)
#endif /* __GNUC__ */
#define ARR_CNT 5
#define CMD_SIZE 50
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

I2C_HandleTypeDef hi2c1;

UART_HandleTypeDef huart2;
UART_HandleTypeDef huart6;

/* USER CODE BEGIN PV */
// CDS 관련 변수
__IO uint16_t ADCxConvertValue = 0;  // 단일 값으로 변경 (배열 대신)
__IO int adcFlag = 0;

// 블루투스 관련 변수
uint8_t rx2char;
volatile unsigned char rx2Flag = 0;
volatile char rx2Data[50];
volatile unsigned char btFlag = 0;
uint8_t btchar;
char btData[50];

// 시간 관련 변수 - 컴파일 시간으로 설정
// 아래 값을 현재 날짜/시간으로 수정하세요:
DATETIME dateTime = {
  .year = (__DATE__[7] - '0') * 1000 + (__DATE__[8] - '0') * 100 + (__DATE__[9] - '0') * 10 + (__DATE__[10] - '0'),  // 현재 연도
  .month = (__DATE__[0] == 'J' && __DATE__[1] == 'a' && __DATE__[2] == 'n') ? 1 :  // 1월
           (__DATE__[0] == 'F') ? 2 :  // 2월
           (__DATE__[0] == 'M' && __DATE__[1] == 'a' && __DATE__[2] == 'r') ? 3 :  // 3월
           (__DATE__[0] == 'A' && __DATE__[1] == 'p') ? 4 :  // 4월
           (__DATE__[0] == 'M' && __DATE__[1] == 'a' && __DATE__[2] == 'y') ? 5 :  // 5월
           (__DATE__[0] == 'J' && __DATE__[1] == 'u' && __DATE__[2] == 'n') ? 6 :  // 6월
           (__DATE__[0] == 'J' && __DATE__[1] == 'u' && __DATE__[2] == 'l') ? 7 :  // 7월
           (__DATE__[0] == 'A' && __DATE__[1] == 'u') ? 8 :  // 8월
           (__DATE__[0] == 'S') ? 9 :  // 9월
           (__DATE__[0] == 'O') ? 10 :  // 10월
           (__DATE__[0] == 'N') ? 11 :  // 11월
           (__DATE__[0] == 'D') ? 12 : 1,  // 12월 (기본값은 1월)
  .day = ((__DATE__[4] >= '0' && __DATE__[4] <= '9') ? (__DATE__[4] - '0') * 10 : 0) + (__DATE__[5] - '0'),  // 현재 일
  .hour = ((__TIME__[0] - '0') * 10) + (__TIME__[1] - '0'),  // 현재 시
  .min = ((__TIME__[3] - '0') * 10) + (__TIME__[4] - '0'),   // 현재 분
  .sec = ((__TIME__[6] - '0') * 10) + (__TIME__[7] - '0')    // 현재 초
};

char timeBuffer[20]; // 시간 표시를 위한 버퍼
uint32_t lastTickValue = 0; // 마지막 Tick 값 저장

// DHT11 센서 관련 변수
DHT11_TypeDef dht11Data;
uint32_t lastDhtReadTick = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_USART6_UART_Init(void);
static void MX_I2C1_Init(void);
static void MX_ADC1_Init(void);
/* USER CODE BEGIN PFP */
void bluetooth_Event();
void UpdateTimeFromSystem(void);
void DisplayDHT11Data(void);
void DisplayTime(void);  // 시간 표시 함수 추가
void ForceTimeUpdate(void);  // 강제 시간 업데이트 함수 추가
void RequestServerTime(void); // 서버 시간 요청 함수
void SyncTimeWithServer(int year, int month, int day, int hour, int min, int sec); // 시간 동기화 함수
char LCD_readChar(void); // LCD 문자 읽기 함수
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/**
 * @brief 시간 표시 함수
 */
void DisplayTime(void) {
  static char prevTimeBuffer[20] = "";  // 이전 시간 문자열 저장

  // 시간 문자열 생성
  sprintf(timeBuffer, "%02d.%02d %02d:%02d:%02d",
          dateTime.month, dateTime.day, dateTime.hour, dateTime.min, dateTime.sec);

  // 이전 표시된 시간과 다를 경우에만 LCD 업데이트
  if(strcmp(prevTimeBuffer, timeBuffer) != 0) {
    // LCD에 시간 표시
    LCD_writeStringXY(0, 0, timeBuffer);

    // 디버그용 - 시간이 업데이트될 때마다 UART로 현재 시간 출력
    printf("Time updated: %s\r\n", timeBuffer);

    // 현재 시간 문자열 저장
    strcpy(prevTimeBuffer, timeBuffer);
  }
}

/**
 * @brief 블록 방식의 시간 업데이트 함수 (메인 루프에서 호출)
 */
void ForceTimeUpdate(void) {
  // 현재 시간 강제 표시
  sprintf(timeBuffer, "%02d.%02d %02d:%02d:%02d",
          dateTime.month, dateTime.day, dateTime.hour, dateTime.min, dateTime.sec);

  LCD_writeStringXY(0, 0, timeBuffer);
  printf("Time forced: %s\r\n", timeBuffer);

  // 매 10초마다 시간이 정상적으로 표시되는지 확인
  static uint32_t lastForceTime = 0;
  uint32_t currentTick = HAL_GetTick();

  if(currentTick - lastForceTime > 10000) {  // 10초마다
    lastForceTime = currentTick;

    // 날짜/시간 정보를 LCD 첫 번째 줄에 다시 표시
    LCD_writeStringXY(0, 0, "                ");  // 첫 줄 지우기
    HAL_Delay(50);
    LCD_writeStringXY(0, 0, timeBuffer);
    printf("Force display time: %s\r\n", timeBuffer);
  }
}

/**
 * @brief 시스템 시간을 dateTime 구조체로 업데이트
 */
void UpdateTimeFromSystem(void) {
  // 현재 시간 가져오기 (HAL_GetTick에 기반한 단순 시간)
  uint32_t currentTick = HAL_GetTick();

  // 1초(1000ms)마다 시간 업데이트
  if(currentTick - lastTickValue >= 1000) {
    lastTickValue = currentTick;

    // 초 증가
    dateTime.sec++;
    if(dateTime.sec >= 60) {
      dateTime.sec = 0;
      dateTime.min++;

      if(dateTime.min >= 60) {
        dateTime.min = 0;
        dateTime.hour++;

        if(dateTime.hour >= 24) {
          dateTime.hour = 0;
          dateTime.day++;

          // 월별 일수 처리 개선
          int daysInMonth;
          switch(dateTime.month) {
            case 2:  // 2월
              // 윤년 처리 (간단히 4로 나누어 떨어지면 윤년으로 처리)
              daysInMonth = ((dateTime.year % 4) == 0) ? 29 : 28;
              break;
            case 4: case 6: case 9: case 11:  // 30일인 달
              daysInMonth = 30;
              break;
            default:  // 그 외 31일인 달
              daysInMonth = 31;
              break;
          }

          if(dateTime.day > daysInMonth) {
            dateTime.day = 1;
            dateTime.month++;

            if(dateTime.month > 12) {
              dateTime.month = 1;
              dateTime.year++;
            }
          }
        }
      }
    }

    // 시간 표시 함수 호출
    DisplayTime();
  }
}

/**
 * @brief DHT11 센서 데이터 읽기 및 LCD에 표시
 */
void DisplayDHT11Data(void) {
  uint32_t currentTick = HAL_GetTick();

  // 2초마다 DHT11 센서 읽기
  if(currentTick - lastDhtReadTick >= 2000) {
    lastDhtReadTick = currentTick;

    // DHT11 센서 데이터 읽기
    dht11Data = DHT11_readData();

    // 온습도 값을 LCD에 표시
    char tempHumiBuffer[32];
    sprintf(tempHumiBuffer, "H:%d%% T:%d.%dC",
            dht11Data.rh_byte1, dht11Data.temp_byte1, dht11Data.temp_byte2);
    LCD_writeStringXY(1, 0, tempHumiBuffer);

    // 디버그용 출력
    printf("CDS=%4d, H:%d%%, T:%d.%dC\r\n",
           ADCxConvertValue, dht11Data.rh_byte1, dht11Data.temp_byte1, dht11Data.temp_byte2);

    // 추가: 서버로 센서 데이터 전송
    char sensorMsg[50];
    sprintf(sensorMsg, "CDS=%d, H:%d, T:%d.%dC\n",
            ADCxConvertValue, dht11Data.rh_byte1, dht11Data.temp_byte1, dht11Data.temp_byte2);
    HAL_UART_Transmit(&huart6, (uint8_t*)sensorMsg, strlen(sensorMsg), 0xFFFF);
  }
}

/**
 * @brief 서버에 시간 요청 함수 (간소화됨)
 */

uint32_t autoSyncInterval = 10000;

void RequestServerTime(void) {
  static uint32_t lastRequestTime = 0;
  uint32_t currentTime = HAL_GetTick();

  // 최소 간격 단축 (2초 -> 1초)
  if(currentTime - lastRequestTime < 1000) {
    return;
  }

  lastRequestTime = currentTime;

  char sendBuf[CMD_SIZE]={0};

  // STM32 ID로 서버에 시간 요청 - 여러 번 전송
  sprintf(sendBuf, "[LJB_STM32]GETTIME@\n");
  for(int i=0; i<2; i++) {
    HAL_UART_Transmit(&huart6, (uint8_t *)sendBuf, strlen(sendBuf), 0xFFFF);
    HAL_Delay(50); // 50ms 대기
  }

  printf("Requesting time from server (multiple attempts)\r\n");

  // LCD에 시간 요청 표시
  LCD_writeStringXY(1, 0, "Syncing time... ");
  HAL_Delay(100);
}

/**
 * @brief LCD에서 현재 글자를 읽는 함수 (문자 구현)
 * 이 함수는 실제로 사용되지 않으며, RequestServerTime 함수를 간소화하여 해결함
 */
char LCD_readChar(void) {
  // 대부분의 LCD 모듈에서는 읽기 기능을 지원하지 않습니다.
  // 따라서 이 함수는 공백 문자를 반환합니다.
  return ' ';
}

/**
 * @brief 서버로부터 받은 시간으로 STM32 시간 동기화
 */
void SyncTimeWithServer(int year, int month, int day, int hour, int min, int sec) {
  // 유효한 값인지 확인
  if(year >= 2000 && year <= 2099 &&
     month >= 1 && month <= 12 &&
     day >= 1 && day <= 31 &&
     hour >= 0 && hour <= 23 &&
     min >= 0 && min <= 59 &&
     sec >= 0 && sec <= 59) {

    // 시간 설정
    dateTime.year = year;
    dateTime.month = month;
    dateTime.day = day;
    dateTime.hour = hour;
    dateTime.min = min;
    dateTime.sec = sec;

    // 시간이 설정되었음을 출력
    printf("Time synchronized: %04d-%02d-%02d %02d:%02d:%02d\r\n",
           dateTime.year, dateTime.month, dateTime.day,
           dateTime.hour, dateTime.min, dateTime.sec);

    // LCD에 즉시 시간 표시
    DisplayTime();
  }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART2_UART_Init();
  MX_USART6_UART_Init();
  MX_I2C1_Init();
  MX_ADC1_Init();
  /* USER CODE BEGIN 2 */
  printf("Start main() - CDS, DHT11, LCD, UART with Time Sync\r\n");

  // UART 인터럽트 시작
  HAL_UART_Receive_IT(&huart2, &rx2char, 1);
  HAL_UART_Receive_IT(&huart6, &btchar, 1);

  // DHT11 센서 초기화
  DHT11_Init();

  // LCD 초기화 및 테스트
  printf("Initializing LCD...\r\n");
  LCD_init(&hi2c1);
  HAL_Delay(100);  // 초기화 후 안정화 시간

  // LCD 테스트 - 간단한 메시지 표시
  LCD_writeStringXY(0, 0, "Initializing...");
  LCD_writeStringXY(1, 0, "Time sync system");
  printf("LCD initialized\r\n");
  HAL_Delay(1000);  // 1초 동안 테스트 메시지 표시

  // LCD 지우기
  LCD_clear();

  // 초기 시간 정보 출력 (디버그용)
  printf("Initial time: %04d-%02d-%02d %02d:%02d:%02d\r\n",
         dateTime.year, dateTime.month, dateTime.day,
         dateTime.hour, dateTime.min, dateTime.sec);

  // ADC 시작
  if(HAL_ADC_Start_IT(&hadc1) != HAL_OK) {
    Error_Handler();
  }

  // 초기화 변수 설정
  lastTickValue = HAL_GetTick();
  lastDhtReadTick = lastTickValue;

  // 초기 시간 표시
  DisplayTime();
  printf("Initial time displayed\r\n");

  // 서버에 시간 요청 - 초기 연결 시도
  HAL_Delay(1000); // 블루투스 안정화를 위한 지연
  RequestServerTime();
  printf("Initial time request sent to server\r\n");

  // 시작 메시지
  LCD_writeStringXY(1, 0, "System ready");
  HAL_Delay(1000);

  printf("Starting main loop...\r\n");
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  uint32_t lastTimeSyncTick = HAL_GetTick(); // 마지막 시간 동기화 시간
  uint32_t autoSyncInterval = 60000; // 시간 동기화 간격 (60초로 시작)
  uint8_t syncFailCount = 0; // 동기화 실패 카운트
  uint8_t syncSuccess = 0;   // 동기화 성공 여부

  while (1)
  {
    // 시간 업데이트 및 표시
    UpdateTimeFromSystem();

    // DHT11 센서 값 읽기 및 표시
    DisplayDHT11Data();

    // ADC(CDS) 값 처리
    if(adcFlag) {
      adcFlag = 0;

      // 다음 ADC 변환 시작
      if(HAL_ADC_Start_IT(&hadc1) != HAL_OK) {
        Error_Handler();
      }
    }

    // UART 메시지 처리
    if(rx2Flag) {
      printf("recv2: %s\r\n", rx2Data);
      rx2Flag = 0;
    }

    // 블루투스 명령 처리 (이 부분에서 시간 업데이트 응답도 처리됨)
    if(btFlag) {
      printf("Received BT data: '%s'\r\n", btData); // 디버그 출력 추가

      // 시간 응답을 받았는지 확인 (정확한 형식 확인)
      if(strncmp(btData, "[GETTIME]", 9) == 0) {
        printf("Time data detected: '%s'\r\n", btData+9); // 시간 데이터 부분 출력

        // 시간 정보 파싱 - 더 유연한 방식으로
        int year=0, month=0, day=0, hour=0, min=0, sec=0;
        char timestr[50] = {0};

        strncpy(timestr, btData+9, sizeof(timestr)-1); // 시간 부분만 복사

        // 시간 데이터에서 개행 문자 제거
        char* newline = strchr(timestr, '\n');
        if(newline) *newline = '\0';

        // 공백으로 요일 부분 분리
        char* daypart = strrchr(timestr, ' ');
        if(daypart) *daypart = '\0'; // 요일 부분 제거

        // 남은 시간 데이터 파싱
        int result = sscanf(timestr, "%d.%d.%d %d:%d:%d",
                            &year, &month, &day, &hour, &min, &sec);

        printf("Parsed: %d.%d.%d %d:%d:%d (matches: %d)\r\n",
               year, month, day, hour, min, sec, result);

        if(result >= 6) {
          // 시간 설정 (2000년대 기준)
          SyncTimeWithServer(2000 + year, month, day, hour, min, sec);
          syncSuccess = 1;
          syncFailCount = 0;

          // 동기화 간격 조정
          if(autoSyncInterval < 600000) {
            autoSyncInterval *= 2;
            if(autoSyncInterval > 600000) {
              autoSyncInterval = 600000;
            }
          }
          printf("Time sync successful. Next sync in %ld seconds\r\n", autoSyncInterval/1000);
        } else {
          printf("Failed to parse time format! (matches: %d)\r\n", result);
        }
      }

      btFlag = 0;
      bluetooth_Event();
    }

    // 주기적으로 서버에 시간 요청
    uint32_t currentTick = HAL_GetTick();
    if(currentTick - lastTimeSyncTick >= autoSyncInterval) {
      lastTimeSyncTick = currentTick;

      // 이전 요청이 실패했는지 확인
      if(!syncSuccess) {
        syncFailCount++;

        // 실패 횟수에 따라 동기화 간격 조정
        if(syncFailCount > 3) {
          // 3회 이상 실패시 간격을 점진적으로 줄임
          autoSyncInterval /= 2;
          if(autoSyncInterval < 30000) {
            autoSyncInterval = 30000; // 최소 30초로 제한
          }
          syncFailCount = 0; // 카운터 초기화
          printf("Sync failed multiple times. Reducing interval to %ld seconds\r\n", autoSyncInterval/1000);
        }
      }

      // 서버에 시간 요청
      RequestServerTime();
      syncSuccess = 0; // 동기화 성공 플래그 초기화
    }

    // 간단한 지연 추가 (시스템 안정화)
    HAL_Delay(10);
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}
/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_1;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_3CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 10000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief USART6 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART6_UART_Init(void)
{

  /* USER CODE BEGIN USART6_Init 0 */

  /* USER CODE END USART6_Init 0 */

  /* USER CODE BEGIN USART6_Init 1 */

  /* USER CODE END USART6_Init 1 */
  huart6.Instance = USART6;
  huart6.Init.BaudRate = 9600;
  huart6.Init.WordLength = UART_WORDLENGTH_8B;
  huart6.Init.StopBits = UART_STOPBITS_1;
  huart6.Init.Parity = UART_PARITY_NONE;
  huart6.Init.Mode = UART_MODE_TX_RX;
  huart6.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart6.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart6) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART6_Init 2 */

  /* USER CODE END USART6_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
/* USER CODE BEGIN MX_GPIO_Init_1 */
/* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(GPIOC, DHT11_Pin|TEST_LED_Pin, GPIO_PIN_RESET);

    /*Configure GPIO pin : B1_Pin */
    GPIO_InitStruct.Pin = B1_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

    /*Configure GPIO pin : LD2_Pin */
    GPIO_InitStruct.Pin = LD2_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);

    /*Configure GPIO pins : DHT11_Pin TEST_LED_Pin */
    GPIO_InitStruct.Pin = DHT11_Pin|TEST_LED_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* USER CODE END MX_GPIO_Init_2 */
  }

  /* USER CODE BEGIN 4 */
  // LED 제어 함수
  void MX_GPIO_LED_ON(int pin)
  {
    HAL_GPIO_WritePin(LD2_GPIO_Port, pin, GPIO_PIN_SET);
  }

  void MX_GPIO_LED_OFF(int pin)
  {
    HAL_GPIO_WritePin(LD2_GPIO_Port, pin, GPIO_PIN_RESET);
  }

  void MX_GPIO_TEST_LED_ON(int pin)
  {
    HAL_GPIO_WritePin(TEST_LED_GPIO_Port, pin, GPIO_PIN_SET);
  }

  void MX_GPIO_TEST_LED_OFF(int pin)
  {
    HAL_GPIO_WritePin(TEST_LED_GPIO_Port, pin, GPIO_PIN_RESET);
  }

  // 블루투스 이벤트 처리 함수
  void bluetooth_Event()
  {
    int i=0;
    char * pToken;
    char * pArray[ARR_CNT]={0};
    char recvBuf[CMD_SIZE]={0};
    char sendBuf[CMD_SIZE]={0};
    strcpy(recvBuf, btData);

    // 이미 시간 메시지를 처리했으면 여기서는 무시
    if(strncmp(btData, "[GETTIME]", 9) == 0) {
      return;
    }

    printf("btData : %s\r\n", btData);

    pToken = strtok(recvBuf, "[@]");
    while(pToken != NULL)
    {
      pArray[i] = pToken;
      if(++i >= ARR_CNT)
        break;
      pToken = strtok(NULL, "[@]");
    }

    if(!strcmp(pArray[1], "LED"))
    {
      if(!strcmp(pArray[2], "ON"))
      {
        MX_GPIO_LED_ON(LD2_Pin);
      }
      else if(!strcmp(pArray[2], "OFF")) // 오타 수정 "1OFF" -> "OFF"
      {
        MX_GPIO_LED_OFF(LD2_Pin);
      }
    }
    else if(!strcmp(pArray[1], "LAMP"))
    {
      if(!strcmp(pArray[2], "ON"))
      {
        MX_GPIO_TEST_LED_ON(TEST_LED_Pin);
      }
      else if(!strcmp(pArray[2], "OFF"))
      {
        MX_GPIO_TEST_LED_OFF(TEST_LED_Pin);
      }
    }
    else if(!strcmp(pArray[1], "TIME"))
    {
      // 서버에 시간 요청
      sprintf(sendBuf, "[%s]GETTIME@\n", pArray[0]);
      HAL_UART_Transmit(&huart6, (uint8_t *)sendBuf, strlen(sendBuf), 0xFFFF);
      printf("Manually requesting server time...\r\n");
      return;
    }
    else if(!strncmp(pArray[1], " New conn", sizeof(" New conn")))
    {
      // 연결 후 자동으로 시간 요청
      HAL_Delay(500); // 연결 안정화를 위한 지연
      sprintf(sendBuf, "[%s]GETTIME@\n", pArray[0]);
      HAL_UART_Transmit(&huart6, (uint8_t *)sendBuf, strlen(sendBuf), 0xFFFF);
      printf("New connection, requesting server time...\r\n");
      return;
    }
    else if(!strncmp(pArray[1], " Already log", sizeof(" Already log")))
    {
      return;
    }
    else
      return;

    sprintf(sendBuf, "[%s]%s@%s\n", pArray[0], pArray[1], pArray[2]);
    HAL_UART_Transmit(&huart6, (uint8_t *)sendBuf, strlen(sendBuf), 0xFFFF);
  }

  /**
    * @brief  Retargets the C library printf function to the USART.
    * @param  None
    * @retval None
    */
  PUTCHAR_PROTOTYPE
  {
    /* Place your implementation of fputc here */
    HAL_UART_Transmit(&huart2, (uint8_t *)&ch, 1, 0xFFFF);
    return ch;
  }

  /**
    * @brief  ADC 변환 완료 콜백 함수
    * @param  AdcHandle: ADC 핸들 포인터
    * @retval None
    */
  void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *AdcHandle)
  {
    ADCxConvertValue = HAL_ADC_GetValue(AdcHandle);
    adcFlag = 1;
  }

  /**
    * @brief  UART 수신 완료 콜백 함수
    * @param  huart: UART 핸들 포인터
    * @retval None
    */
  void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
  {
    if(huart->Instance == USART2)
    {
      static int i=0;
      rx2Data[i] = rx2char;
      if((rx2Data[i] == '\r')||(rx2Data[i] == '\n'))
      {
        rx2Data[i] = '\0';
        rx2Flag = 1;
        i = 0;
      }
      else
      {
        i++;
      }
      HAL_UART_Receive_IT(&huart2, &rx2char, 1);
    }
    else if(huart->Instance == USART6)  // 이 부분을 else if로 변경
    {
      static int i=0;
      // 버퍼 오버플로우 방지 체크 추가
      if(i >= CMD_SIZE-1) {
        i = 0; // 버퍼 오버플로우 방지
        btFlag = 1; // 강제로 처리 트리거
      }

      btData[i] = btchar;
      if((btData[i] == '\n') || btData[i] == '\r')
      {
        btData[i] = '\0';
        btFlag = 1;
        i = 0;
      }
      else
      {
        i++;
      }
      HAL_UART_Receive_IT(&huart6, &btchar, 1);
    }
  }
  /* USER CODE END 4 */

  /**
    * @brief  This function is executed in case of error occurrence.
    * @retval None
    */
  void Error_Handler(void)
  {
    /* USER CODE BEGIN Error_Handler_Debug */
    /* User can add his own implementation to report the HAL error return state */
    __disable_irq();
    while (1)
    {
      printf("Error_Handler\r\n");
    }
    /* USER CODE END Error_Handler_Debug */
  }

  #ifdef  USE_FULL_ASSERT
  /**
    * @brief  Reports the name of the source file and the source line number
    *         where the assert_param error has occurred.
    * @param  file: pointer to the source file name
    * @param  line: assert_param error line source number
    * @retval None
    */
  void assert_failed(uint8_t *file, uint32_t line)
  {
    /* USER CODE BEGIN 6 */
    /* User can add his own implementation to report the file name and line number,
       ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
    /* USER CODE END 6 */
  }
  #endif /* USE_FULL_ASSERT */
