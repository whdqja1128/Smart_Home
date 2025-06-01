#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
#include <mysql/mysql.h>
#include <stdbool.h>
#include <time.h>
#include <ctype.h>

#define BUF_SIZE 200
#define NAME_SIZE 20
#define ARR_CNT 5

// 모드 상태 정의
#define MODE_AUTO 0
#define MODE_MANUAL 1

void* send_msg(void* arg);
void* recv_msg(void* arg);
void* motor_check_thread(void* arg);
void* environment_monitor(void* arg);
void* sensor_data_collection(void* arg);
void calculate_optimal_conditions(MYSQL* conn);
double calculate_discomfort(double temp, double humi);
int get_illumination();
double get_temperature();
double get_humidity();
void error_handling(char* msg);

char name[NAME_SIZE] = "[Default]";
char msg[BUF_SIZE];

// 전역 변수
volatile bool monitoring_active = false;
volatile int last_motor_value = 0;
volatile int cds_threshold = 300; // 조도 센서 임계값
volatile int operation_mode = MODE_AUTO; // 기본값은 Auto Mode
volatile double auto_temp_threshold = 28.0; // 온도 임계값
volatile double auto_humi_threshold = 50.0; // 습도 임계값
volatile double discomfort_threshold = 70.0; // 불쾌지수 임계값
volatile int real_illumination = 0;
volatile double real_temperature = 0.0;
volatile double real_humidity = 0.0;
volatile bool real_sensor_data_available = false;
pthread_mutex_t sensor_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t monitor_mutex = PTHREAD_MUTEX_INITIALIZER;

// MYSQL 연결 정보
char* host = "localhost";
char* user = "iot";
char* pass = "pwiot";
char* dbname = "iotdb";

typedef struct {
    int* sock;
    MYSQL* conn;
} THREAD_ARG;

int main(int argc, char* argv[])
{
    int sock;
    struct sockaddr_in serv_addr;
    pthread_t snd_thread, rcv_thread, check_thread, env_thread, sensor_thread;
    void* thread_return;
    MYSQL* conn;
    THREAD_ARG* thread_arg;

    if (argc != 4) {
        printf("Usage : %s <IP> <port> <name>\n", argv[0]);
        exit(1);
    }

    sprintf(name, "%s", argv[3]);

    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock == -1)
        error_handling("socket() error");

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
    serv_addr.sin_port = htons(atoi(argv[2]));

    if (connect(sock, (struct sockaddr*) & serv_addr, sizeof(serv_addr)) == -1)
        error_handling("connect() error");

    // 비밀번호 없이 ID만 전송
    sprintf(msg, "[%s]", name);
    write(sock, msg, strlen(msg));

    // MySQL 연결 초기화
    conn = mysql_init(NULL);
    puts("MYSQL startup");
    if (!(mysql_real_connect(conn, host, user, pass, dbname, 0, NULL, 0)))
    {
        fprintf(stderr, "ERROR : %s[%d]\n", mysql_error(conn), mysql_errno(conn));
        exit(1);
    }
    else
        printf("Connection Successful!\n\n");
        
    // log 테이블이 없으면 생성
    char sql_cmd[200];
    sprintf(sql_cmd, "CREATE TABLE IF NOT EXISTS log ("
                "id INT AUTO_INCREMENT PRIMARY KEY, "
                "date DATE, "
                "time TIME, "
                "temp FLOAT, "
                "humi FLOAT, "
                "discomfort_index FLOAT"
                ")");
    if (mysql_query(conn, sql_cmd)) {
        fprintf(stderr, "ERROR creating log table: %s[%d]\n", mysql_error(conn), mysql_errno(conn));
    } else {
        printf("Checked/created log table\n");
    }

    // 스레드 인자 준비
    thread_arg = (THREAD_ARG*)malloc(sizeof(THREAD_ARG));
    thread_arg->sock = &sock;
    thread_arg->conn = conn;

    // 모터 상태 확인 스레드 생성
    pthread_create(&check_thread, NULL, motor_check_thread, (void*)thread_arg);
    
    // 환경 모니터링 스레드 생성 (Auto 모드용)
    pthread_create(&env_thread, NULL, environment_monitor, (void*)thread_arg);
    
    // 센서 데이터 수집 스레드 생성
    pthread_create(&sensor_thread, NULL, sensor_data_collection, (void*)thread_arg);
    
    // 메시지 수신/송신 스레드 생성
    pthread_create(&rcv_thread, NULL, recv_msg, (void*)thread_arg);
    pthread_create(&snd_thread, NULL, send_msg, (void*)&sock);

    pthread_join(snd_thread, &thread_return);
    pthread_join(rcv_thread, &thread_return);
    pthread_join(check_thread, &thread_return);
    pthread_join(env_thread, &thread_return);
    pthread_join(sensor_thread, &thread_return);

    mysql_close(conn);
    free(thread_arg);
    close(sock);
    return 0;
}

// 불쾌지수 계산 함수
double calculate_discomfort(double temp, double humi) {
    return 1.8 * temp - 0.55 * (1 - humi/100) * (1.8 * temp - 26) + 32;
}

// 환경 모니터링 스레드 함수 (Auto 모드에서 사용)
void* environment_monitor(void* arg) {
    THREAD_ARG* thread_arg = (THREAD_ARG*)arg;
    int* sock = thread_arg->sock;
    MYSQL* conn = thread_arg->conn;
    char sql_cmd[200];
    char motor_msg[BUF_SIZE];
    time_t motor_start_time = 0;    // 모터 시작 시간 기록
    bool motor_running = false;     // 모터 실행 상태 추적
    
    printf("Environment monitor thread started\n");
    
    while(1) {
        // Auto Mode일 때만 환경 기반 제어
        pthread_mutex_lock(&monitor_mutex);
        bool is_auto_mode = (operation_mode == MODE_AUTO);
        pthread_mutex_unlock(&monitor_mutex);
        
        if (is_auto_mode) {
            // 현재 시간 확인
            time_t current_time = time(NULL);
            
            // 모터가 실행 중이고 30분(1800초)이 지났는지 확인
            if (motor_running && (current_time - motor_start_time >= 1800)) {
                // 모터 정지 명령 전송
                sprintf(motor_msg, "[LJB_LIN]MOTOR@OFF\n");
                write(*sock, motor_msg, strlen(motor_msg));
                printf("AUTO mode: Motor turned OFF after 30 minutes\n");
                
                pthread_mutex_lock(&monitor_mutex);
                monitoring_active = false;
                last_motor_value = 0;
                pthread_mutex_unlock(&monitor_mutex);
                
                // 모터 상태 업데이트
                motor_running = false;
                
                // 최적 조건 계산 및 저장
                calculate_optimal_conditions(conn);
                
                // 다음 확인까지 10초 대기 후 계속
                sleep(10);
                continue;
            }
            
            // 조도 센서 값 확인
            int cds_value = get_illumination();
            
            if (cds_value < cds_threshold) {
                // 최근 최적 조건 로드
                double optimal_temp = auto_temp_threshold;
                double optimal_humi = auto_humi_threshold;
                double optimal_discomfort = discomfort_threshold;
                
                // 현재 환경 조건 확인
                double temp = get_temperature();
                double humi = get_humidity();
                double discomfort = calculate_discomfort(temp, humi);
                
                printf("Auto mode check: CDS=%d, Temp=%.1f, Humi=%.1f, Discomfort=%.1f\n",
                    cds_value, temp, humi, discomfort);
                
                // *** 중요: AUTO 모드에서는 환경 데이터를 확인만 하고 INSERT하지 않음 ***
                // 확인용 메시지 추가
                printf("AUTO mode: Monitoring environment conditions without database INSERT\n");
                
                // 모터 작동 조건 확인 (모터가 이미 실행 중이 아닐 때만)
                if (!motor_running && 
                    (temp >= optimal_temp || humi >= optimal_humi || 
                     discomfort >= optimal_discomfort)) {
                    
                    // 모터 ON 명령으로 작동 (100% 속도로)
                    pthread_mutex_lock(&monitor_mutex);
                    monitoring_active = true;
                    last_motor_value = 1;
                    pthread_mutex_unlock(&monitor_mutex);
                    
                    // 서버에 모터 제어 메시지 전송
                    sprintf(motor_msg, "[LJB_LIN]MOTOR@ON\n");
                    write(*sock, motor_msg, strlen(motor_msg));
                    
                    printf("AUTO mode: Motor turned ON due to environmental conditions\n");
                    
                    // 모터 시작 시간 기록 및 상태 업데이트
                    motor_start_time = time(NULL);
                    motor_running = true;
                }
            }
        } else {
            // MANUAL 모드일 때는 메시지만 출력
            printf("MANUAL mode - Environment monitor idle\n");
        }
        
        // 10초마다 확인
        sleep(10);
    }
    
    return NULL;
}

// 모터 상태 확인 스레드 함수
void* motor_check_thread(void* arg) {
    THREAD_ARG* thread_arg = (THREAD_ARG*)arg;
    int* sock = thread_arg->sock;
    MYSQL* conn = thread_arg->conn;
    char sql_cmd[200];
    int motor_value = 0;
    int motor_speed = 0;
    MYSQL_RES* result = NULL;
    MYSQL_ROW row;
    
    printf("Motor check thread started\n");
    
    while(1) {
        // MOTOR 장치의 값 조회
        sprintf(sql_cmd, "SELECT value, speed FROM device WHERE name='MOTOR'");
        
        if (mysql_query(conn, sql_cmd)) {
            fprintf(stderr, "MySQL Error: %s\n", mysql_error(conn));
            sleep(3);
            continue;
        }
        
        result = mysql_store_result(conn);
        if (result == NULL) {
            fprintf(stderr, "MySQL Error: %s\n", mysql_error(conn));
            sleep(3);
            continue;
        }
        
        row = mysql_fetch_row(result);
        if (row && row[0]) {
            motor_value = atoi(row[0]);
            if (row[1]) {
                motor_speed = atoi(row[1]);
            }
            
            pthread_mutex_lock(&monitor_mutex);
            
            if (motor_value == 1 && last_motor_value == 0) {
                // 모터가 켜진 경우 - 센서 모니터링 시작
                monitoring_active = true;
                printf("Motor ON (speed: %d) - Starting sensor monitoring\n", motor_speed);
            } 
            else if (motor_value == 0 && last_motor_value == 1) {
                // 모터가 꺼진 경우 - 센서 모니터링 중지 및 최적값 계산
                monitoring_active = false;
                printf("Motor OFF - Stopping sensor monitoring\n");
                
                // 결과셋을 해제하고 나서 calculate_optimal_conditions 호출
                mysql_free_result(result);
                result = NULL;
                
                // 최적 환경 조건 계산 및 저장
                calculate_optimal_conditions(conn);
                
                // 다음 반복으로 진행 (다음 쿼리 실행 전 이미 결과셋이 해제되었음)
                last_motor_value = motor_value;
                pthread_mutex_unlock(&monitor_mutex);
                sleep(3);
                continue;
            }
            
            last_motor_value = motor_value;
            pthread_mutex_unlock(&monitor_mutex);
        }
        
        if (result != NULL) {
            mysql_free_result(result);
            result = NULL;
        }
        
        // 3초마다 확인
        sleep(3);
    }
    
    return NULL;
}

// 최적 환경 조건 계산 함수
void calculate_optimal_conditions(MYSQL* conn) {
    char sql_cmd[500];
    MYSQL_RES* result = NULL;
    MYSQL_ROW row;
    double avg_temp = 0.0;
    double avg_humi = 0.0;
    double discomfort = 0.0;
    int row_count = 0;
    
    printf("Calculating optimal conditions...\n");
    
    // 모든 센서 데이터에서 온도와 습도의 평균값 계산
    sprintf(sql_cmd, "SELECT AVG(temp), AVG(humi), COUNT(*) FROM sensor");
    
    if (mysql_query(conn, sql_cmd)) {
        fprintf(stderr, "MySQL Error: %s\n", mysql_error(conn));
        return;
    }
    
    result = mysql_store_result(conn);
    if (result == NULL) {
        fprintf(stderr, "MySQL Error: %s\n", mysql_error(conn));
        return;
    }
    
    // 평균값 가져오기
    row = mysql_fetch_row(result);
    if (row && row[0] && row[1] && row[2]) {
        avg_temp = atof(row[0]);
        avg_humi = atof(row[1]);
        row_count = atoi(row[2]);
        
        // 불쾌지수 계산
        discomfort = calculate_discomfort(avg_temp, avg_humi);
    }
    
    mysql_free_result(result);
    result = NULL;
    
    // 데이터가 없으면 기본값 사용
    if (row_count == 0) {
        avg_temp = 30.0;
        avg_humi = 60.0;
        discomfort = calculate_discomfort(avg_temp, avg_humi);
        printf("No sensor data found, using default values\n");
    }
    
    printf("Calculated averages: Temp=%.1f, Humi=%.1f, Discomfort=%.1f (from %d records)\n", 
        avg_temp, avg_humi, discomfort, row_count);
    
    // 최적값 저장 (log 테이블에 평균값 기록)
    sprintf(sql_cmd, "INSERT INTO log (date, time, temp, humi, discomfort_index) "
            "VALUES (CURDATE(), CURTIME(), %.1f, %.1f, %.1f)", 
            avg_temp, avg_humi, discomfort);
    
    if (mysql_query(conn, sql_cmd)) {
        fprintf(stderr, "MySQL Error on INSERT: %s\n", mysql_error(conn));
    } else {
        printf("Stored average conditions in log table\n");
    }
    
    // 모든 센서 데이터 삭제 (테이블 초기화)
    sprintf(sql_cmd, "TRUNCATE TABLE sensor");
    
    if (mysql_query(conn, sql_cmd)) {
        fprintf(stderr, "MySQL Error on TRUNCATE: %s\n", mysql_error(conn));
    } else {
        printf("Cleared sensor table\n");
    }
    
    // Auto Mode 임계값 업데이트
    auto_temp_threshold = avg_temp;
    auto_humi_threshold = avg_humi;
    discomfort_threshold = discomfort;
    
    printf("Updated thresholds: Temp=%.1f, Humi=%.1f, Discomfort=%.1f\n", 
        auto_temp_threshold, auto_humi_threshold, discomfort_threshold);
}

// 센서 데이터 수집 함수
void* sensor_data_collection(void* arg) {
    THREAD_ARG* thread_arg = (THREAD_ARG*)arg;
    int* sock = thread_arg->sock;
    MYSQL* conn = thread_arg->conn;
    char sql_cmd[200];
    char name_msg[BUF_SIZE];
    
    printf("Sensor data collection thread started\n");
    
    while(1) {
        // 현재 동작 모드 확인
        pthread_mutex_lock(&monitor_mutex);
        bool is_manual_mode = (operation_mode == MODE_MANUAL);
        pthread_mutex_unlock(&monitor_mutex);
        
        // MANUAL 모드에서만 데이터 수집 및 저장 수행
        if (is_manual_mode) {
            // 센서 데이터 획득
            int illu;
            double temp, humi;
            
            pthread_mutex_lock(&sensor_mutex);
            if (real_sensor_data_available) {
                illu = real_illumination;
                temp = real_temperature;
                humi = real_humidity;
            } else {
                // 데이터가 없으면 기본값 사용
                illu = 800;  // 기본값
                temp = 30.0; // 기본값 
                humi = 60.0; // 기본값
                printf("Warning: Using default sensor values\n");
            }
            pthread_mutex_unlock(&sensor_mutex);
            
            // 센서 데이터 저장 (MANUAL 모드에서만 실행)
            // 한 번 더 명확하게 모드 확인 (중요한 체크)
            pthread_mutex_lock(&monitor_mutex);
            is_manual_mode = (operation_mode == MODE_MANUAL);
            pthread_mutex_unlock(&monitor_mutex);
            
            if (is_manual_mode) {
                char sql_cmd[200];
                sprintf(sql_cmd, "INSERT INTO sensor (name, date, time, illu, temp, humi) "
                        "VALUES ('%s', CURDATE(), CURTIME(), %d, %.1f, %.1f)", 
                        name, illu, temp, humi);
                
                if (mysql_query(conn, sql_cmd)) {
                    fprintf(stderr, "MySQL Error: %s[%d]\n", mysql_error(conn), mysql_errno(conn));
                } else {
                    printf("Inserted sensor data (MANUAL mode): illu=%d, temp=%.1f, humi=%.1f\n", 
                          illu, temp, humi);
                    
                    // 서버에 센서 데이터 전송
                    char name_msg[BUF_SIZE];
                    sprintf(name_msg, "[ALLMSG]Sensor data (MANUAL mode): illu=%d, temp=%.1f, humi=%.1f\n",
                            illu, temp, humi);
                    write(*sock, name_msg, strlen(name_msg));
                }
            } else {
                printf("Mode changed to AUTO during data collection - skipping INSERT\n");
            }
        } else {
            // AUTO 모드일 때는 메시지만 출력하고 INSERT는 하지 않음
            printf("AUTO mode active - Skipping regular sensor data collection\n");
        }
        
        // 수집 주기
        sleep(10);
    }
    
    return NULL;
}

void* send_msg(void* arg)
{
    int* sock = (int*)arg;
    int str_len;
    int ret;
    fd_set initset, newset;
    struct timeval tv;
    char name_msg[NAME_SIZE + BUF_SIZE + 2];

    FD_ZERO(&initset);
    FD_SET(STDIN_FILENO, &initset);

    fputs("Input a message! [ID]msg (Default ID:ALLMSG)\n", stdout);
    while (1) {
        memset(msg, 0, sizeof(msg));
        name_msg[0] = '\0';
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        newset = initset;
        ret = select(STDIN_FILENO + 1, &newset, NULL, NULL, &tv);
        if (FD_ISSET(STDIN_FILENO, &newset))
        {
            fgets(msg, BUF_SIZE, stdin);
            if (!strncmp(msg, "quit\n", 5)) {
                *sock = -1;
                return NULL;
            }
            else if (msg[0] != '[')
            {
                strcat(name_msg, "[ALLMSG]");
                strcat(name_msg, msg);
            }
            else
                strcpy(name_msg, msg);
            if (write(*sock, name_msg, strlen(name_msg)) <= 0)
            {
                *sock = -1;
                return NULL;
            }
        }
        if (ret == 0)
        {
            if (*sock == -1)
                return NULL;
        }
    }
}

void* recv_msg(void* arg)
{
    THREAD_ARG* thread_arg = (THREAD_ARG*)arg;
    int* sock = thread_arg->sock;
    MYSQL* conn = thread_arg->conn;
    int i;
    char* pToken;
    char* pArray[ARR_CNT] = { 0 };

    char name_msg[NAME_SIZE + BUF_SIZE + 1];
    int str_len;
    
    while (1) {
        memset(name_msg, 0x0, sizeof(name_msg));
        str_len = read(*sock, name_msg, NAME_SIZE + BUF_SIZE);
        if (str_len <= 0)
        {
            *sock = -1;
            return NULL;
        }
        name_msg[str_len] = 0;
        fputs(name_msg, stdout);
        
        // MODE_CHECK 요청 처리 - 현재 모드를 서버에 알려줌
        if (strstr(name_msg, "MODE_CHECK") != NULL) {
            pthread_mutex_lock(&monitor_mutex);
            bool is_manual_mode = (operation_mode == MODE_MANUAL);
            pthread_mutex_unlock(&monitor_mutex);
            
            // 현재 모드 정보를 서버에 전송
            char mode_info[BUF_SIZE];
            sprintf(mode_info, "[%s]MODE_STATUS@%s\n", name, 
                    is_manual_mode ? "MANUAL" : "AUTO");
            write(*sock, mode_info, strlen(mode_info));
            
            printf("Sent mode status: %s", mode_info);
            continue;
        }
        
        // 간소화된 센서 데이터 형식 처리 (추가)
        // 형식: "illu temp humi"
        int illu = 0;
        double temp = 0.0;
        double humi = 0.0;
        
        if (sscanf(name_msg, "%d %lf %lf", &illu, &temp, &humi) == 3) {
            printf("Received simplified sensor data: illu=%d, temp=%.1f, humi=%.1f\n", 
                   illu, temp, humi);
            
            // 온도 보정 적용
            temp = (int)(temp * 0.95 + 0.5);
            
            // 센서 값 직접 업데이트
            pthread_mutex_lock(&sensor_mutex);
            real_illumination = illu;
            real_temperature = temp;
            real_humidity = humi;
            real_sensor_data_available = true;
            pthread_mutex_unlock(&sensor_mutex);
            
            // AUTO 모드에서는 INSERT 하지 않음
            pthread_mutex_lock(&monitor_mutex);
            bool is_manual_mode = (operation_mode == MODE_MANUAL);
            pthread_mutex_unlock(&monitor_mutex);
            
            if (is_manual_mode) {
                // MANUAL 모드일 때만 데이터베이스에 삽입
                char sql_cmd[200];
                sprintf(sql_cmd, "INSERT INTO sensor (name, date, time, illu, temp, humi) "
                        "VALUES (\"%s\", now(), now(), %d, %lf, %lf)", 
                        name, illu, temp, humi);
                
                int res = mysql_query(conn, sql_cmd);
                if (res) {
                    fprintf(stderr, "MySQL Error on INSERT: %s[%d]\n", mysql_error(conn), mysql_errno(conn));
                } else {
                    // INSERT는 결과셋이 없지만, 혹시 모를 상황에 대비
                    MYSQL_RES* result = mysql_store_result(conn);
                    if (result != NULL) {
                        mysql_free_result(result);
                    }
                    printf("Inserted %lu rows from simplified format: illu=%d, temp=%.1f, humi=%.1f\n", 
                           (unsigned long)mysql_affected_rows(conn), illu, temp, humi);
                }
            } else {
                printf("AUTO mode: Not inserting sensor data into database\n");
            }
                
            // 간소화된 데이터 형식은 이미 처리했으므로 다음 메시지로 넘어감
            continue;
        }
        
        // 여기서부터 기존 메시지 파싱 처리
        pToken = strtok(name_msg, "[:@]");
        i = 0;
        while (pToken != NULL) {
            pArray[i] = pToken;
            if (++i >= ARR_CNT)
                break;
            pToken = strtok(NULL, "[:@]");
        }
        
        // SENSOR 데이터 처리
        if(i >= 2 && !strcmp(pArray[1], "SENSOR") && (i == 5)){
            int illu = atoi(pArray[2]);
            // 2번 파일처럼 온도 보정 적용
            double temp = (int)(atof(pArray[3]) * 0.95 + 0.5);
            double humi = atof(pArray[4]);
            
            // 추가: 실제 센서 값 업데이트
            pthread_mutex_lock(&sensor_mutex);
            real_illumination = illu;
            real_temperature = temp;
            real_humidity = humi;
            real_sensor_data_available = true;
            pthread_mutex_unlock(&sensor_mutex);
            
            // AUTO 모드에서는 INSERT 하지 않음 (중요한 체크)
            pthread_mutex_lock(&monitor_mutex);
            bool is_manual_mode = (operation_mode == MODE_MANUAL);
            pthread_mutex_unlock(&monitor_mutex);
            
            if (is_manual_mode) {
                char sql_cmd[200];
                sprintf(sql_cmd, "INSERT INTO sensor (name, date, time, illu, temp, humi) "
                        "VALUES (\"%s\", now(), now(), %d, %lf, %lf)", 
                        pArray[0], illu, temp, humi);
                
                int res = mysql_query(conn, sql_cmd);
                if (!res)
                    printf("Inserted %lu rows with real sensor data: illu=%d, temp=%.1f, humi=%.1f\n", 
                           (unsigned long)mysql_affected_rows(conn), illu, temp, humi);
                else
                    fprintf(stderr, "ERROR: %s[%d]\n", mysql_error(conn), mysql_errno(conn));
            } else {
                printf("AUTO mode: Not inserting sensor data into database\n");
            }
        }
        // CDS 포맷의 센서 데이터 처리
        else if (strstr(name_msg, "CDS=") != NULL) {
            printf("DEBUG - Received sensor data: %s\n", name_msg);
        
            int illu = 0;
            double humi = 0.0;
            double temp = 0.0;
        
            // 조도 (CDS) 값 파싱
            sscanf(name_msg, "CDS=%d", &illu);
        
            // 습도 (H:) 값 파싱 - %를 무시하고 숫자만 추출
            char humi_str[10];
            char *humi_ptr = strstr(name_msg, "H:");
            if (humi_ptr) {
                if (sscanf(humi_ptr, "H:%[^%]", humi_str) == 1) {
                    humi = atof(humi_str);  // 문자열을 double로 변환
                }
                printf("Extracted humidity: %.1f\n", humi);
            }
        
            // 온도 (T:) 값 파싱 - C를 무시하고 숫자만 추출
            char temp_str[10];
            char *temp_ptr = strstr(name_msg, "T:");
            if (temp_ptr) {
                if (sscanf(temp_ptr, "T:%[^C]", temp_str) == 1) {
                    temp = atof(temp_str);  // 문자열을 double로 변환
                }
                printf("Extracted temperature: %.1f\n", temp);
            }
        
            printf("Final parsed values: illu=%d, humi=%.1f, temp=%.1f\n", illu, humi, temp);
            
            // 온도 보정 적용
            temp = (int)(temp * 0.95 + 0.5);
            
            // 센서 값 직접 업데이트
            pthread_mutex_lock(&sensor_mutex);
            real_illumination = illu;
            real_temperature = temp;
            real_humidity = humi;
            real_sensor_data_available = true;
            pthread_mutex_unlock(&sensor_mutex);
            
            printf("Updated sensor data: illu=%d, temp=%.1f, humi=%.1f\n", 
                   illu, temp, humi);
            
            // AUTO 모드에서는 INSERT 하지 않음 (중요한 체크)
            pthread_mutex_lock(&monitor_mutex);
            bool is_manual_mode = (operation_mode == MODE_MANUAL);
            pthread_mutex_unlock(&monitor_mutex);
            
            if (is_manual_mode) {
                // 센서 데이터를 바로 DB에 저장
                char sql_cmd[200];
                sprintf(sql_cmd, "INSERT INTO sensor (name, date, time, illu, temp, humi) "
                        "VALUES (\"%s\", now(), now(), %d, %lf, %lf)", 
                        name, illu, temp, humi);
                
                int res = mysql_query(conn, sql_cmd);
                if (!res)
                    printf("Inserted %lu rows from CDS format: illu=%d, temp=%.1f, humi=%.1f\n", 
                           (unsigned long)mysql_affected_rows(conn), illu, temp, humi);
                else
                    fprintf(stderr, "ERROR: %s[%d]\n", mysql_error(conn), mysql_errno(conn));
            } else {
                printf("AUTO mode: Not inserting sensor data into database\n");
            }
        }
        
        // 모드 변경 명령 처리
        else if (i >= 3 && !strcmp(pArray[1], "MODE")) {
            printf("Mode command from %s: %s\n", pArray[0], pArray[2]);  // 디버그 출력 추가
            if (!strcmp(pArray[2], "MANUAL")) {
                pthread_mutex_lock(&monitor_mutex);
                operation_mode = MODE_MANUAL;
                monitoring_active = true; // MANUAL 모드에서는 모니터링 활성화
                pthread_mutex_unlock(&monitor_mutex);
                printf("Switched to MANUAL mode by %s - Sensor data collection enabled\n", pArray[0]);
            } else if (!strcmp(pArray[2], "AUTO")) {
                pthread_mutex_lock(&monitor_mutex);
                operation_mode = MODE_AUTO;
                monitoring_active = false; // AUTO 모드에서는 자동 제어
                pthread_mutex_unlock(&monitor_mutex);
                printf("Switched to AUTO mode - Sensor data collection by environment monitor\n");
            }
        }
        // 모터 관련 명령 처리
        else if (i >= 3 && !strcmp(pArray[1], "MOTOR")) {
            // 여기서는 MOTOR 명령을 통한 모드 전환이 발생합니다.
            
            // MOTOR@ON 명령 처리 (추가)
            if (!strcmp(pArray[2], "ON")) {
                // Auto 모드로 전환 및 모니터링 활성화
                pthread_mutex_lock(&monitor_mutex);
                operation_mode = MODE_AUTO;
                monitoring_active = true;
                last_motor_value = 100;  // ON은 100% 속도
                pthread_mutex_unlock(&monitor_mutex);
                
                printf("Switched to AUTO mode via MOTOR@ON. Motor at 100%\n");
            }
            // MOTOR@OFF 명령 처리 (추가)
            else if (!strcmp(pArray[2], "OFF") || !strcmp(pArray[2], "0")) {
                // Auto 모드로 전환 및 모니터링 중지
                pthread_mutex_lock(&monitor_mutex);
                operation_mode = MODE_AUTO;
                monitoring_active = false;
                last_motor_value = 0;
                pthread_mutex_unlock(&monitor_mutex);
                
                // 최적 조건 계산 및 저장
                calculate_optimal_conditions(conn);
                
                printf("Switched to AUTO mode via MOTOR@OFF. Motor stopped.\n");
            }
            // 100 명령어도 ON과 동일하게 처리 (추가)
            else if (!strcmp(pArray[2], "100")) {
                // Auto 모드로 전환 및 모니터링 활성화
                pthread_mutex_lock(&monitor_mutex);
                operation_mode = MODE_AUTO;  // 중요: 여기를 AUTO로 유지
                monitoring_active = true;
                last_motor_value = 100;
                pthread_mutex_unlock(&monitor_mutex);
                
                printf("Switched to AUTO mode via MOTOR@100. Motor at 100%\n");
            }
            // REFRESH 명령어 특별 처리 (추가)
            else if (!strcmp(pArray[2], "REFRESH")) {
                // Manual 모드로 전환 - 모니터링은 활성화 (Auto 모드에서는 REFRESH 사용 불가)
                pthread_mutex_lock(&monitor_mutex);
                operation_mode = MODE_MANUAL;  // REFRESH는 MANUAL 모드로 설정
                monitoring_active = true;
                last_motor_value = 80;  // REFRESH는 약 80% 속도
                pthread_mutex_unlock(&monitor_mutex);
                
                printf("Switched to MANUAL mode via MOTOR@REFRESH. Reverse motor at 80%\n");
            }
            // 기존 숫자 명령 처리
            else {
                int motor_value = atoi(pArray[2]);
                
                if (motor_value > 0) {
                    // Manual 모드로 전환 및 모니터링 활성화
                    pthread_mutex_lock(&monitor_mutex);
                    operation_mode = MODE_MANUAL;
                    monitoring_active = true;
                    last_motor_value = motor_value;
                    pthread_mutex_unlock(&monitor_mutex);
                    
                    printf("Switched to MANUAL mode. Motor speed: %d\n", motor_value);
                } else {
                    // Auto 모드로 전환 및 모니터링 중지
                    pthread_mutex_lock(&monitor_mutex);
                    operation_mode = MODE_AUTO;
                    monitoring_active = false;
                    last_motor_value = 0;
                    pthread_mutex_unlock(&monitor_mutex);
                    
                    // 최적 조건 계산 및 저장
                    calculate_optimal_conditions(conn);
                    
                    printf("Switched to AUTO mode. Motor stopped.\n");
                }
            }
        }
        // 임계값 설정 명령 처리
        else if (i >= 4 && !strcmp(pArray[1], "THRESHOLD")) {
            if (!strcmp(pArray[2], "TEMP")) {
                auto_temp_threshold = atof(pArray[3]);
                printf("Temperature threshold set to %.1f\n", auto_temp_threshold);
            }
            else if (!strcmp(pArray[2], "HUMI")) {
                auto_humi_threshold = atof(pArray[3]);
                printf("Humidity threshold set to %.1f\n", auto_humi_threshold);
            }
            else if (!strcmp(pArray[2], "DISCOMFORT")) {
                discomfort_threshold = atof(pArray[3]);
                printf("Discomfort threshold set to %.1f\n", discomfort_threshold);
            }
            else if (!strcmp(pArray[2], "CDS")) {
                cds_threshold = atoi(pArray[3]);
                printf("CDS threshold set to %d\n", cds_threshold);
            }
        }
    }
    
    return NULL;
}

// 온도 값 얻기 (시뮬레이션)
int get_illumination() {
    int value;
    pthread_mutex_lock(&sensor_mutex);
    // 실제 센서 데이터가 있으면 그 값을 사용, 없으면 기본값 반환
    if (real_sensor_data_available) {
        value = real_illumination;
    } else {
        // 기본값 또는 이전 방식의 시뮬레이션 값 반환
        value = 500;  // 기본값 설정
    }
    pthread_mutex_unlock(&sensor_mutex);
    return value;
}

double get_temperature() {
    double value;
    pthread_mutex_lock(&sensor_mutex);
    if (real_sensor_data_available) {
        value = real_temperature;
    } else {
        value = 25.0;  // 기본값 설정
    }
    pthread_mutex_unlock(&sensor_mutex);
    return value;
}

double get_humidity() {
    double value;
    pthread_mutex_lock(&sensor_mutex);
    if (real_sensor_data_available) {
        value = real_humidity;
    } else {
        value = 50.0;  // 기본값 설정
    }
    pthread_mutex_unlock(&sensor_mutex);
    return value;
}

void error_handling(char* msg)
{
    fputs(msg, stderr);
    fputc('\n', stderr);
    exit(1);
}