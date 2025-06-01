/* author : KSH (Modified for better time synchronization) */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>

#define BUF_SIZE 100
#define MAX_CLNT 32
#define ID_SIZE 10
#define ARR_CNT 5

#define DEBUG
typedef struct {
    char fd;
    char *from;
    char *to;
    char *msg;
    int len;
} MSG_INFO;

typedef struct {
    int index;
    int fd;
    char ip[20];
    char id[ID_SIZE];
} CLIENT_INFO;

void * clnt_connection(void * arg);
void send_msg(MSG_INFO * msg_info, CLIENT_INFO * first_client_info);
void error_handling(char * msg);
void log_file(char * msgstr);
void getlocaltime(char * buf);
void process_time_request(int fd, char *client_id, char *time_data);

int clnt_cnt=0;
pthread_mutex_t mutx;

int main(int argc, char *argv[])
{
    int serv_sock, clnt_sock;
    struct sockaddr_in serv_adr, clnt_adr;
    int clnt_adr_sz;
    int sock_option  = 1;
    pthread_t t_id[MAX_CLNT] = {0};
    int str_len = 0;
    int i;
    char idinfo[ID_SIZE+3]; // ID만 있는 형식으로 변경
    char *pToken;
    char *pArray[ARR_CNT]={0};
    char msg[BUF_SIZE];

    CLIENT_INFO client_info[MAX_CLNT] = { {0,-1,"","LJB_SQL"}, \
            {0,-1,"","LJB_BT"},  {0,-1,"","LJB_STM32"}, \
            {0,-1,"","LJB_LIN"}, {0,-1,"","LJB_AND"}, \
            {0,-1,"","LJB_ARD"}, {0,-1,"","HM_CON"}};
            // 순서대로 SQL, 블루투스, STM, 리눅스, 안드로이드, 아두이노

    if(argc != 2) {
        printf("Usage : %s <port>\n",argv[0]);
        exit(1);
    }
    fputs("IoT Server Start!!\n",stdout);

    if(pthread_mutex_init(&mutx, NULL))
        perror("mutex");

    serv_sock = socket(PF_INET, SOCK_STREAM, 0);

    memset(&serv_adr, 0, sizeof(serv_adr));
    serv_adr.sin_family=AF_INET;
    serv_adr.sin_addr.s_addr=htonl(INADDR_ANY);
    serv_adr.sin_port=htons(atoi(argv[1]));

    setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, (void*)&sock_option, sizeof(sock_option));
    if(bind(serv_sock, (struct sockaddr *)&serv_adr, sizeof(serv_adr))==-1)
        perror("bind");

    if(listen(serv_sock, 5) == -1)
        perror("listen");

    while(1) {
        clnt_adr_sz = sizeof(clnt_adr);
        clnt_sock = accept(serv_sock, (struct sockaddr *)&clnt_adr, &clnt_adr_sz);
        if(clnt_cnt >= MAX_CLNT)
        {
            printf("socket full\n");
            shutdown(clnt_sock,SHUT_WR);
            continue;
        }
        else if(clnt_sock < 0)
        {
            perror("accept");
            continue;
        }

        // 클라이언트로부터 ID 정보만 받기
        str_len = read(clnt_sock, idinfo, sizeof(idinfo));
        idinfo[str_len] = '\0';

        if(str_len > 0)
        {
            i=0;
            // 메시지 형식이 [ID] 형태로 변경됨
            pToken = strtok(idinfo,"[]");

            // 토큰이 하나만 필요함 (ID)
            if(pToken != NULL)
            {
                pArray[0] = pToken;
                
                // 사전 정의된 ID 목록과 비교
                for(i=0; i<MAX_CLNT; i++)
                {
                    if(!strcmp(client_info[i].id, pArray[0]))
                    {
                        // 이미 연결된 ID인 경우
                        if(client_info[i].fd != -1)
                        {
                            sprintf(msg,"[%s] Already logged!\n", pArray[0]);
                            write(clnt_sock, msg, strlen(msg));
                            log_file(msg);
                            shutdown(clnt_sock, SHUT_WR);
#if 1   // MCU 연결을 위한 처리
                            client_info[i].fd = -1;
#endif  
                            break;
                        }
                        
                        // 비밀번호 검증 제거 - ID만으로 연결 허용
                        // 클라이언트 정보 업데이트
                        strcpy(client_info[i].ip, inet_ntoa(clnt_adr.sin_addr));
                        
                        pthread_mutex_lock(&mutx);
                        client_info[i].index = i; 
                        client_info[i].fd = clnt_sock; 
                        clnt_cnt++;
                        pthread_mutex_unlock(&mutx);
                        
                        sprintf(msg,"[%s] New connected! (ip:%s,fd:%d,sockcnt:%d)\n",
                                pArray[0], inet_ntoa(clnt_adr.sin_addr), clnt_sock, clnt_cnt);
                        log_file(msg);
                        write(clnt_sock, msg, strlen(msg));

                        pthread_create(t_id+i, NULL, clnt_connection, (void *)(client_info + i));
                        pthread_detach(t_id[i]);
                        break;
                    }
                }
                
                // 일치하는 ID가 없는 경우
                if(i == MAX_CLNT)
                {
                    sprintf(msg,"[%s] Authentication Error!\n", pArray[0]);
                    write(clnt_sock, msg, strlen(msg));
                    log_file(msg);
                    shutdown(clnt_sock, SHUT_WR);
                }
            }
            else 
            {
                // ID 형식이 잘못된 경우
                sprintf(msg,"ID format error!\n");
                write(clnt_sock, msg, strlen(msg));
                log_file(msg);
                shutdown(clnt_sock, SHUT_WR);
            }
        }
        else 
            shutdown(clnt_sock, SHUT_WR);
    }
    return 0;
}

void * clnt_connection(void * arg)
{
    CLIENT_INFO * client_info = (CLIENT_INFO *)arg;
    int str_len = 0;
    int index = client_info->index;
    char msg[BUF_SIZE];
    char to_msg[MAX_CLNT*ID_SIZE+1];
    int i=0;
    char *pToken;
    char *pArray[ARR_CNT]={0};
    char strBuff[130]={0};

    MSG_INFO msg_info;
    CLIENT_INFO  * first_client_info;

    first_client_info = (CLIENT_INFO *)((void *)client_info - (void *)( sizeof(CLIENT_INFO) * index ));
    
    // 새 연결 시 STM32 클라이언트에게 자동으로 시간 전송 (개선된 부분)
    if(strcmp(client_info->id, "LJB_STM32") == 0) {
        char timeBuf[50] = {0};
        getlocaltime(timeBuf);
        
        printf("Sending initial time to STM32: %s", timeBuf);
        write(client_info->fd, timeBuf, strlen(timeBuf));
        
        // 1초 후 한 번 더 전송 (안정성 확보)
        sleep(1);
        getlocaltime(timeBuf);
        write(client_info->fd, timeBuf, strlen(timeBuf));
    }
    
    while(1)
    {
        memset(msg,0x0,sizeof(msg));
        str_len = read(client_info->fd, msg, sizeof(msg)-1); 
        if(str_len <= 0)
            break;

        msg[str_len] = '\0';

        if(strcmp(client_info->id, "LJB_BT") == 0 && strstr(msg, "CDS=") != NULL) {
            // 원본 메시지를 그대로 전송하지 말고, 명확한 형식으로 변환하여 전송
            int illu = 0;
            float humi = 0.0;
            float temp = 0.0;
    
            // 원본 메시지 파싱
            char *humi_ptr = strstr(msg, "H:");
            char *temp_ptr = strstr(msg, "T:");
    
            sscanf(msg, "CDS=%d", &illu);
    
            if(humi_ptr) {
                if(sscanf(humi_ptr, "H:%f", &humi) != 1) {
                    // % 문자 없이 시도
                    sscanf(humi_ptr, "H:%f", &humi);
                }
            }
    
            if(temp_ptr) {
                if(sscanf(temp_ptr, "T:%fC", &temp) != 1) {
                    // C 문자 없이 시도
                    sscanf(temp_ptr, "T:%f", &temp);
                }
            }
    
            // 센서 데이터를 명확한 형식으로 변환
            char sensor_data[BUF_SIZE];
            sprintf(sensor_data, "CDS=%d, H:%.1f, T:%.1fC", illu, humi, temp);
    
            printf("Parsed from original: CDS=%d, H:%.1f%%, T:%.1fC\n", illu, humi, temp);
    
            // LJB_SQL로 전송
            for(i=0; i<MAX_CLNT; i++) {
                if((first_client_info+i)->fd != -1) {
                    if(!strcmp((first_client_info+i)->id, "LJB_SQL")) {
                        write((first_client_info+i)->fd, sensor_data, strlen(sensor_data));
                        printf("Forwarded formatted sensor data to LJB_SQL: %s\n", sensor_data);
                        break;
                    }
                }
            }
            continue;
        }

        pToken = strtok(msg,"[:]");
        i = 0; 
        while(pToken != NULL)
        {
            pArray[i] =  pToken;
            if(i++ >= ARR_CNT)
                break;	
            pToken = strtok(NULL,"[:]");
        }

        if(i >= 3 && !strcmp(pArray[1], "MODE_STATUS")) {
            // 모드 상태 정보를 저장
            char* status = pArray[2];
            printf("Received mode status from %s: %s\n", client_info->id, status);
            
            // AUTO 모드일 경우 센서 데이터를 전송하지 않도록 플래그 설정 가능
            // 여기에서는 단순히 로깅만 처리
            log_file("Mode status update received");
            continue;
        }

        // GETTIME 명령 처리 개선 (직접 처리)
        if(i >= 2 && !strcmp(pArray[1], "GETTIME")) {
            // GETTIME@YY.MM.DD HH:MM:SS 형식 확인
            char *client_time = (i >= 3) ? pArray[2] : NULL;
            process_time_request(client_info->fd, client_info->id, client_time);
            continue;
        }

        int illu;
        float temp, humi;
        if (sscanf(msg, "%d %f %f", &illu, &temp, &humi) == 3) {
            // 센서 데이터로 감지된 경우, 그대로 SQL 클라이언트에게 전달
            printf("Detected numeric sensor data: %s\n", msg);
            for(i=0; i<MAX_CLNT; i++) {
                if((first_client_info+i)->fd != -1) {
                    if(!strcmp((first_client_info+i)->id, "LJB_SQL")) {
                        write((first_client_info+i)->fd, msg, strlen(msg));
                        printf("Forwarded raw sensor data to LJB_SQL: %s\n", msg);
                        break;
                    }
                }
            }
        } else {
            // 일반 메시지로 처리
            msg_info.fd = client_info->fd;
            msg_info.from = client_info->id;
            msg_info.to = pArray[0];
            sprintf(to_msg,"[%s]%s",msg_info.from,pArray[1]);
            msg_info.msg = to_msg;
            msg_info.len = strlen(to_msg);
            
            sprintf(strBuff,"msg : [%s->%s] %s",msg_info.from,msg_info.to,pArray[1]);
            log_file(strBuff);
            send_msg(&msg_info, first_client_info);
        }
    }

    close(client_info->fd);

    sprintf(strBuff,"Disconnect ID:%s (ip:%s,fd:%d,sockcnt:%d)\n",client_info->id,client_info->ip,client_info->fd,clnt_cnt-1);
    log_file(strBuff);

    pthread_mutex_lock(&mutx);
    clnt_cnt--;
    client_info->fd = -1;
    pthread_mutex_unlock(&mutx);

    return 0;
}

// 개선된 시간 요청 처리 함수
void process_time_request(int fd, char *client_id, char *time_data) {
    char timeBuf[50] = {0};
    getlocaltime(timeBuf);
    
    // STM32 클라이언트인 경우 더 명확한 응답 전송
    if(strcmp(client_id, "LJB_STM32") == 0) {
        // 로그 기록
        printf("Sending time to STM32: %s", timeBuf);
        log_file("Sending time to STM32");
        
        // 응답 전송 - 여러 번 시도하여 확실히 전달
        for(int i=0; i<3; i++) {
            write(fd, timeBuf, strlen(timeBuf));
            usleep(100000); // 100ms 대기
        }
    } else {
        // 다른 클라이언트 처리
        write(fd, timeBuf, strlen(timeBuf));
    }
}

void send_msg(MSG_INFO * msg_info, CLIENT_INFO * first_client_info)
{
    int i=0;
    char motor_cmd[BUF_SIZE];
    bool is_motor_cmd = false;
    int motor_speed = 0;
    
    // 중복 메시지 처리 방지를 위한 정적 변수
    static char last_arduino_cmd[BUF_SIZE] = {0};
    static char last_sql_cmd[BUF_SIZE] = {0};
    static unsigned long last_arduino_time = 0;
    static unsigned long last_sql_time = 0;
    static int arduino_dup_count = 0;
    static int sql_dup_count = 0;
    static bool arduino_cmd_blocked = false;
    static bool sql_cmd_blocked = false;
    static unsigned long arduino_block_end_time = 0;
    static unsigned long sql_block_end_time = 0;
    
    // 현재 시간 가져오기 (밀리초 단위)
    struct timeval tv;
    gettimeofday(&tv, NULL);
    unsigned long current_time = tv.tv_sec * 1000 + tv.tv_usec / 1000;
    
    // 명령 차단 해제 확인
    if (arduino_cmd_blocked && current_time > arduino_block_end_time) {
        arduino_cmd_blocked = false;
        arduino_dup_count = 0;
    }
    
    if (sql_cmd_blocked && current_time > sql_block_end_time) {
        sql_cmd_blocked = false;
        sql_dup_count = 0;
    }
    
    // 현재 함수 호출에서 이미 전송했는지 확인하는 플래그
    bool already_sent_to_arduino = false;
    bool already_sent_to_sql = false;
    
    // MOTOR 명령어 처리를 위한 특별 로직
    if (strstr(msg_info->msg, "MOTOR@") != NULL) {
        // 모터 명령어 감지
        is_motor_cmd = true;
        
        // 아두이노에서 보낸 응답인지 확인하여 처리 방지
        if (strcmp(msg_info->from, "LJB_ARD") == 0 && strstr(msg_info->to, "LJB_ARD") != NULL) {
            // 아두이노에서 아두이노로 보내는 응답 메시지는 무시
            printf("Ignored self-response from Arduino\n");
            return;
        }
        
        // ON 명령 처리 (추가된 부분)
        if (strstr(msg_info->msg, "MOTOR@ON") != NULL) {
            motor_speed = 100; // 100% 속도
            
            // MOTOR@ON은 AUTO 모드 명령 전송 (명확하게 전송)
            sprintf(motor_cmd, "[%s]MODE@AUTO\n", msg_info->from);
        }
        // OFF 명령 처리 (기존의 OFF 처리 확장)
        else if (strstr(msg_info->msg, "MOTOR@OFF") != NULL || strstr(msg_info->msg, "MOTOR@0") != NULL) {
            motor_speed = 0;
            
            // MOTOR@OFF는 AUTO 모드 명령 전송 (명확하게 전송)
            sprintf(motor_cmd, "[%s]MODE@AUTO\n", msg_info->from);
        }
        // 속도값 추출 (MOTOR@60 같은 형식에서 60 추출) - 기존 부분
        else {
            char *speed_str = strstr(msg_info->msg, "MOTOR@") + 6;
            if (strcmp(speed_str, "REFRESH\n") == 0) {
                motor_speed = 80; // REFRESH는 80% 속도로 처리
                // MOTOR@REFRESH인 경우 MODE@MANUAL 명령 전송
                sprintf(motor_cmd, "[%s]MODE@MANUAL\n", msg_info->from);
            } else {
                motor_speed = atoi(speed_str);
                // 일반 속도 명령인 경우 MODE@MANUAL 명령 전송
                sprintf(motor_cmd, "[%s]MODE@MANUAL\n", msg_info->from);
            }
        }
        
        // SQL 클라이언트에 모드 변경 메시지 전송
        for(i=0; i<MAX_CLNT; i++) {
            if((first_client_info+i)->fd != -1) {
                if(!strcmp((first_client_info+i)->id, "LJB_SQL")) {
                    // 모드 메시지 먼저 전송
                    write((first_client_info+i)->fd, motor_cmd, strlen(motor_cmd));
                    printf("Sent mode command to SQL: %s", motor_cmd);
                    usleep(100000); // 100ms 대기 (명령 처리 시간 확보)
                    
                    // 모터 명령 전송 - ON/OFF도 적절히 처리
                    if (strstr(msg_info->msg, "MOTOR@ON") != NULL) {
                        sprintf(motor_cmd, "[%s]MOTOR@100\n", msg_info->from); // ON을 100으로 변환
                    } else if (strstr(msg_info->msg, "MOTOR@OFF") != NULL) {
                        sprintf(motor_cmd, "[%s]MOTOR@0\n", msg_info->from); // OFF를 0으로 변환
                    } else {
                        sprintf(motor_cmd, "[%s]MOTOR@%d\n", msg_info->from, motor_speed);
                    }
                    
                    write((first_client_info+i)->fd, motor_cmd, strlen(motor_cmd));
                    printf("Sent motor command to SQL: %s", motor_cmd);
                    break;
                }
            }
        }
    }

    // 메시지 라우팅 처리
    if(!strcmp(msg_info->to,"ALLMSG"))
    {
        for(i=0; i<MAX_CLNT; i++) {
            if((first_client_info+i)->fd != -1) {
                // 만약 MOTOR 관련 메시지이고 SQL이나 아두이노에서 왔다면 중복 방지
                if (is_motor_cmd && 
                    (strcmp(msg_info->from, "LJB_SQL") == 0 || 
                     strcmp(msg_info->from, "LJB_ARD") == 0)) {
                    // 같은 디바이스에는 에코백 방지
                    if (strcmp((first_client_info+i)->id, msg_info->from) == 0) {
                        continue;
                    }
                }
                write((first_client_info+i)->fd, msg_info->msg, msg_info->len);
            }
        }
    }

    // CDS= 형식의 센서 데이터 감지 및 라우팅 (수정된 부분)
    else if (strcmp(msg_info->from, "LJB_BT") == 0 && strstr(msg_info->msg, "CDS=") != NULL) {
        // 센서 데이터 파싱
        int illu = 0;
        float humi = 0.0;
        float temp = 0.0;
        
        // 디버그용
        printf("Original sensor message: %s\n", msg_info->msg);
        
        // CDS 값 파싱
        sscanf(msg_info->msg, "CDS=%d", &illu);
        
        // 습도 파싱 - 여러 가능한 형식 고려
        char *humi_ptr = strstr(msg_info->msg, "H:");
        if(humi_ptr) {
            char humi_str[20] = {0};
            int i = 0;
            humi_ptr += 2; // "H:" 다음으로 이동
            
            // 숫자나 소수점만 복사
            while((*humi_ptr >= '0' && *humi_ptr <= '9') || *humi_ptr == '.') {
                humi_str[i++] = *humi_ptr++;
                if(i >= 19) break;
            }
            humi_str[i] = '\0';
            
            if(strlen(humi_str) > 0) {
                humi = atof(humi_str);
                printf("Extracted humidity: %s -> %.1f\n", humi_str, humi);
            }
        }
        
        // 온도 파싱 - 여러 가능한 형식 고려
        char *temp_ptr = strstr(msg_info->msg, "T:");
        if(temp_ptr) {
            char temp_str[20] = {0};
            int i = 0;
            temp_ptr += 2; // "T:" 다음으로 이동
            
            // 숫자나 소수점만 복사
            while((*temp_ptr >= '0' && *temp_ptr <= '9') || *temp_ptr == '.') {
                temp_str[i++] = *temp_ptr++;
                if(i >= 19) break;
            }
            temp_str[i] = '\0';
            
            if(strlen(temp_str) > 0) {
                temp = atof(temp_str);
                printf("Extracted temperature: %s -> %.1f\n", temp_str, temp);
            }
        }
        
        // 값 검증 - 모든 값이 있는지 확인
        if(illu >= 0 && humi >= 0.0 && temp >= 0.0) {
            // 단순 숫자만 전송 - "illu temp humi" 형식
            char simple_data[BUF_SIZE];
            sprintf(simple_data, "%d %.1f %.1f\n", illu, temp, humi);
            
            printf("Simplified sensor data: %s", simple_data);
            
            // SQL 클라이언트에 간소화된 형식으로 전송
            // AUTO 모드인지 확인하기 위해 SQL 클라이언트에게 쿼리 메시지 전송
            char mode_check[BUF_SIZE];
            sprintf(mode_check, "[%s]MODE_CHECK\n", msg_info->from);
            
            for(i=0; i<MAX_CLNT; i++) {
                if((first_client_info+i)->fd != -1) {
                    if(!strcmp((first_client_info+i)->id, "LJB_SQL")) {
                        // 모드 체크를 위한 메시지 전송
                        write((first_client_info+i)->fd, mode_check, strlen(mode_check));
                        usleep(10000); // 10ms 대기
                        
                        // 센서 데이터 전송
                        write((first_client_info+i)->fd, simple_data, strlen(simple_data));
                        printf("Forwarded simplified sensor data to LJB_SQL\n");
                        break;
                    }
                }
            }
        } else {
            printf("Parsing error or invalid values: illu=%d, temp=%.1f, humi=%.1f\n", 
                   illu, temp, humi);
        }
    }
    
    else if(!strcmp(msg_info->to,"IDLIST"))
    {
        // IDLIST 처리
        char idlist[BUF_SIZE*2] = {0};
        sprintf(idlist, "[IDLIST]");
        
        for(i=0; i<MAX_CLNT; i++) {
            if((first_client_info+i)->fd != -1) {
                strcat(idlist, (first_client_info+i)->id);
                strcat(idlist, ":");
            }
        }
        
        // 마지막 콜론 제거
        if(strlen(idlist) > 9) { // "[IDLIST]" 길이는 9
            idlist[strlen(idlist)-1] = '\n';
        } else {
            strcat(idlist, "\n");
        }
        
        write(msg_info->fd, idlist, strlen(idlist));
    }
    else if(!strcmp(msg_info->to,"GETTIME"))
    {
        // 시간 요청 처리 - 직접 처리하지 않고 process_time_request 함수 호출
        process_time_request(msg_info->fd, msg_info->from, NULL);
    }
    else
    {
        // MOTOR 명령어일 경우 특별 처리 - 모든 소스에서 처리하도록 수정
        if (is_motor_cmd) {
            // SQL 클라이언트에 동일한 형식의 메시지 전송
            sprintf(motor_cmd, "[%s]MOTOR@%d\n", msg_info->from, motor_speed);
            
            for(i=0; i<MAX_CLNT; i++) {
                if((first_client_info+i)->fd != -1) {
                    if(!strcmp((first_client_info+i)->id, "LJB_SQL")) {
                        write((first_client_info+i)->fd, motor_cmd, strlen(motor_cmd));
                        break;
                    }
                }
            }
        }

        if(!strcmp(msg_info->to, "LJB_ARD"))
        {
            for(i=0; i<MAX_CLNT; i++)
            {
                if((first_client_info+i)->fd != -1)
                {
                    if(!strcmp((first_client_info+i)->id, "LJB_ARD"))
                    {
                        write((first_client_info+i)->fd, msg_info->msg, msg_info->len);
                        break;
                    }
                }
            }
        }

        // 일반적인 메시지 전송 (MOTOR 명령이 아닌 경우)
        else {
            // 수신자에게 메시지 전송
            for(i=0; i<MAX_CLNT; i++)
                if((first_client_info+i)->fd != -1)    
                    if(!strcmp(msg_info->to,(first_client_info+i)->id))
                        write((first_client_info+i)->fd, msg_info->msg, msg_info->len);
        }
    }
}

void error_handling(char *msg)
{
    fputs(msg, stderr);
    fputc('\n', stderr);
    exit(1);
}

void log_file(char * msgstr)
{
    fputs(msgstr, stdout);
    fputc('\n', stdout);
}

void getlocaltime(char * buf)
{
    struct tm *t;
    time_t tt;
    char wday[7][4] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    tt = time(NULL);
    if(errno == EFAULT)
        perror("time()");
    t = localtime(&tt);
    
    // STM32에 맞는 형식으로 포맷팅: [GETTIME]YY.MM.DD HH:MM:SS Day
    sprintf(buf,"[GETTIME]%02d.%02d.%02d %02d:%02d:%02d %s\n",
            t->tm_year+1900-2000, t->tm_mon+1, t->tm_mday,
            t->tm_hour, t->tm_min, t->tm_sec, wday[t->tm_wday]);
    return;
}