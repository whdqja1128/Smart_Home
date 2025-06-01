#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>

#define BUF_SIZE 100
#define NAME_SIZE 20
#define ARR_CNT 5

void * send_msg(void * arg);
void * recv_msg(void * arg);
void error_handling(char * msg);
void process_button_command(int* sock, char* cmd);

char name[NAME_SIZE]="[Default]";

typedef struct {
	int sockfd;	
	int btfd;	
	char sendid[NAME_SIZE];
}DEV_FD;

// 버튼 누름 순서에 따른 모터 속도 설정
int button_speed_levels[] = {40, 70, 100, 0}; // 0은 OFF
int button_press_count = 0;

int main(int argc, char *argv[])
{
	DEV_FD dev_fd;
	struct sockaddr_in serv_addr;
	pthread_t snd_thread, rcv_thread;
	void * thread_return;
	int ret;
	struct sockaddr_rc addr = { 0 };
//  	char dest[18] = "98:D3:11:F8:4A:AA";	//iot00
  	char dest[18] = "98:DA:60:09:9B:C8";	//EMB00 by home_ksh
	char msg[BUF_SIZE];

	if(argc != 4) {
		printf("Usage : %s <IP> <port> <name>\n",argv[0]);
		exit(1);
	}

	sprintf(name, "%s",argv[3]);

	dev_fd.sockfd = socket(PF_INET, SOCK_STREAM, 0);
	if(dev_fd.sockfd == -1)
		error_handling("socket() error");

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family=AF_INET;
	serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
	serv_addr.sin_port = htons(atoi(argv[2]));

	if(connect(dev_fd.sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1)
		error_handling("connect() error");

	// PASSWD 인증 제거하고 단순 ID 식별자만 전송
	sprintf(msg,"[%s]",name);
	write(dev_fd.sockfd, msg, strlen(msg));

	dev_fd.btfd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
	if(dev_fd.btfd == -1){
		perror("socket()");
		exit(1);
	}

	// set the connection parameters (who to connect to)
	addr.rc_family = AF_BLUETOOTH;
	addr.rc_channel = (uint8_t)1;
	str2ba(dest, &addr.rc_bdaddr);

	ret = connect(dev_fd.btfd, (struct sockaddr *)&addr, sizeof(addr));
	if(ret == -1){
		perror("connect()");
		exit(1);
	}

	pthread_create(&rcv_thread, NULL, recv_msg, (void *)&dev_fd);
	pthread_create(&snd_thread, NULL, send_msg, (void *)&dev_fd);

	pthread_join(snd_thread, &thread_return);
	//	pthread_join(rcv_thread, &thread_return);

	close(dev_fd.sockfd);
	return 0;
}

void normalize_sensor_data(char* input, char* output) {
    int illu = 0;
    float temp = 0.0;
    float humi = 0.0;
    
    // CDS 값 파싱
    char* cds_ptr = strstr(input, "CDS=");
    if(cds_ptr) {
        sscanf(cds_ptr, "CDS=%d", &illu);
    }
    
    // 습도 값 파싱
    char* humi_ptr = strstr(input, "H:");
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
        }
    }
    
    // 온도 값 파싱
    char* temp_ptr = strstr(input, "T:");
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
        }
    }
    
    // 단순 숫자 형식으로 출력 생성
    sprintf(output, "%d %.1f %.1f", illu, temp, humi);
}

void * send_msg(void * arg)  //bluetooth --> server
{
	DEV_FD *dev_fd = (DEV_FD *)arg;
	int str_len;
	int ret;
	fd_set initset, newset;
	struct timeval tv;
	char name_msg[NAME_SIZE + BUF_SIZE+2];
	char msg[BUF_SIZE];
	int total=0;

	FD_ZERO(&initset);
	FD_SET(dev_fd->sockfd, &initset);
	FD_SET(dev_fd->btfd, &initset);

	//	fputs("Input a message! [ID]msg (Default ID:ALLMSG)\n",stdout);
	while(1) {
		//		memset(msg,0,sizeof(msg));
		//		name_msg[0] = '\0';
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		newset = initset;
		ret = select(dev_fd->btfd + 1, &newset, NULL, NULL, &tv);
		//        if(FD_ISSET(STDIN_FILENO, &newset))
		if(FD_ISSET(dev_fd->btfd, &newset))
		{
			ret=read(dev_fd->btfd, msg+total,BUF_SIZE-total);
			if(ret > 0)
			{
				total += ret;
			}
			else if(ret == 0) {
				dev_fd->sockfd = -1;
				return NULL;
			}
		
			if(msg[total-1] == '\n')
			{
				msg[total]=0;
				total = 0;
			}
			else
				continue;
		
			fputs("ARD:", stdout);
			fputs(msg, stdout);
			
			// 버튼 명령어 처리 추가
			if (strncmp(msg, "BUTTON", 6) == 0) {
				process_button_command(&(dev_fd->sockfd), msg);
			} 
			else if (strstr(msg, "CDS=") != NULL) {
				// 센서 데이터 정규화
				char normalized_data[BUF_SIZE];
				normalize_sensor_data(msg, normalized_data);
				
				printf("Normalized sensor data: %s\n", normalized_data);
				
				// 정규화된 데이터 전송
				if(write(dev_fd->sockfd, normalized_data, strlen(normalized_data))<=0)
				{
					dev_fd->sockfd = -1;
					return NULL;
				}
			}
			else {
				// 일반 메시지 전송
				if(write(dev_fd->sockfd, msg, strlen(msg))<=0)
				{
					dev_fd->sockfd = -1;
					return NULL;
				}
			}
		}
		if(ret == 0) 
		{
			if(dev_fd->sockfd == -1) 
				return NULL;
		}
	}
}

// 버튼 명령 처리 함수
void process_button_command(int* sock, char* cmd) {
    char msg_to_send[BUF_SIZE];
    
    // 버튼 누름 횟수에 따라 모터 속도 설정
    button_press_count = (button_press_count + 1) % 4; // 0, 1, 2, 3 순환
    int speed = button_speed_levels[button_press_count];
    
    printf("Button pressed: Count=%d, Speed=%d\n", button_press_count, speed);
    
    // 모드 변경 (Manual 모드로)
    sprintf(msg_to_send, "[LJB_LIN]MODE@MANUAL\n");
    write(*sock, msg_to_send, strlen(msg_to_send));
    
    // 모터 제어 명령 전송
    if (speed > 0) {
        // ON 명령은 100으로 전송 (MOTOR@100)
        if (speed == 100) {
            sprintf(msg_to_send, "[LJB_LIN]MOTOR@ON\n");
        } else {
            sprintf(msg_to_send, "[LJB_LIN]MOTOR@%d\n", speed);
        }
    } else {
        // OFF 명령은 MOTOR@OFF로 전송
        sprintf(msg_to_send, "[LJB_LIN]MOTOR@OFF\n");
    }
    write(*sock, msg_to_send, strlen(msg_to_send));
}

void * recv_msg(void * arg)  //server --> bluetooth
{
	DEV_FD *dev_fd = (DEV_FD *)arg;
	int i;
	char *pToken;
	char *pArray[ARR_CNT]={0};

	char name_msg[NAME_SIZE + BUF_SIZE +1];
	int str_len;
	
	while(1) {
		memset(name_msg,0x0,sizeof(name_msg));
		str_len = read(dev_fd->sockfd, name_msg, NAME_SIZE + BUF_SIZE);
		if(str_len <= 0) 
		{
			dev_fd->sockfd = -1;
			return NULL;
		}
		name_msg[str_len] = 0;
  		fputs("SRV:",stdout);
		fputs(name_msg, stdout);

		write(dev_fd->btfd,name_msg,strlen(name_msg));   
	}
}

void error_handling(char * msg)
{
	fputs(msg, stderr);
	fputc('\n', stderr);
	exit(1);
}