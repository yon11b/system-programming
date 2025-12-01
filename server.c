#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <time.h>

#define ROWS 64
#define COLS 64
#define N (ROWS*COLS)       // 전체 데이터 수: 64x64=4096
#define SM_COUNT 8          // 클라이언트 수
#define SERVER_COUNT 4      // 서버 수
#define CHUNK 128           // 메시지 하나에 담을 최대 int 개수 (큐 전송 단위)

// 메시지 큐 구조체
struct msgbuf { 
    long mtype;            // 메시지 타입 (여기서는 1로 통일)
    int count;             // 메시지 안의 int 개수, 0이면 서버 종료 신호
    int data[CHUNK];       // 실제 int 데이터
};

// 시간 측정 함수
double now_sec() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec/1e9;
}

int main() {
    int qids[SERVER_COUNT];

    // 1. 서버별 메시지 큐 생성
    // ftok로 키 생성 → msgget으로 큐 생성
    for(int s=0;s<SERVER_COUNT;s++)
        qids[s]=msgget(1574,IPC_CREAT|0666);

    pid_t sids[SERVER_COUNT];

    // 2. 서버 프로세스 생성
    for(int s=0;s<SERVER_COUNT;s++) {
        if(fork()==0){
            // 서버 파일 이름
            char *fname[4]={"server0.dat","server1.dat","server2.dat","server3.dat"};
            FILE *f=fopen(fname[s],"ab");  // 서버 파일 열기(append 모드)
            struct msgbuf msg;
            long total_ints=0;
            double total_io_time=0.0;

            // 서버 메시지 수신 루프
            while(1){
                double t0 = now_sec();                                      // 수신 시작 시간
                msgrcv(qids[s],&msg,sizeof(msg)-sizeof(long),0,0);          // 메시지 수신
                double t1 = now_sec();                                      // 수신 후 시간
                total_io_time += (t1-t0);                                   // 시간 누적

                if(msg.count==0) break;                                     // count==0이면 종료 신호
                fwrite(msg.data,sizeof(int),msg.count,f);                  // 받은 데이터를 파일에 기록
                total_ints += msg.count;
            }

            fclose(f); // 파일 닫기
            // 서버 처리 요약 출력
            printf("[SERVER %d] received %ld ints, total_io_time=%.6f s\n",
                   s,total_ints,total_io_time);
            fflush(stdout);
            exit(0);   // 서버 종료
        }
    }

    usleep(200000); // 서버 준비를 위한 짧은 대기

    // 3. 클라이언트 프로세스 생성
    for(int c=0;c<SM_COUNT;c++){
        if(fork()==0){
            /*
                start: 0    / end: 512
                start: 512  / end: 1024
                start: 1024 / end: 1536
                start: 1536 / end: 2048
                start: 2048 / end: 2560
                start: 2560 / end: 3072
                start: 3072 / end: 3584
                start: 3584 / end: 4096
            */
            int start=c*(N/SM_COUNT), end=start+(N/SM_COUNT); // 클라이언트가 담당할 데이터 범위
            int *bufs[SERVER_COUNT];       // 서버별 임시 버퍼
            int counts[SERVER_COUNT]={0};  // 각 서버별 데이터 개수

            // 서버별 버퍼 메모리 할당
            for(int s=0;s<SERVER_COUNT;s++)
                bufs[s]=malloc(sizeof(int)*(end-start));

            // 3-1. 데이터 생성 및 서버별 분류(RAID0 스트라이프)
            double t0_g = now_sec();  // 데이터 생성 시작 시간
            for(int i=start;i<end;i++){
                int srv=(i/128)%SERVER_COUNT; // RAID0 계산: 어느 서버로 보낼지 결정

                int pos = counts[srv];  // 서버 srv에 몇 번째로 넣을지 저장
                bufs[srv][pos] = i;     // 해당 위치에 데이터 저장
                counts[srv] = counts[srv] + 1;  // 데이터 개수 1 증가
            }
            double t1_g = now_sec();  // 데이터 생성 종료 시간
            double gather_time = t1_g - t0_g;

            // 3-2. 클라이언트 -> 서버별 메시지 큐로 CHUNK 단위 전송
            double t0_s = now_sec(); // 전송 시작 시간
            for(int s=0;s<SERVER_COUNT;s++){
                int sent=0;             // 지금까지 보낸 데이터 개수
                while(sent<counts[s]){      // counts[s]: 서버 s에 보내야 하는 데이터 총 개수
                    int take=counts[s]-sent;
                    if(take>CHUNK)
                        take=CHUNK; // CHUNK 단위 전송
                    struct msgbuf msg; msg.mtype=1; msg.count=take;

                    for(int k=0;k<take;k++)
                        msg.data[k]=bufs[s][sent+k]; // 데이터 복사
                    msgsnd(qids[s],&msg,sizeof(msg.count)+take*sizeof(int),0); // 큐에 전송
                    sent+=take;
                }
            }
            double t1_s = now_sec(); // 전송 종료 시간
            double send_time = t1_s - t0_s;

            // 3-3. 메모리 해제 후 클라이언트 종료
            for(int s=0;s<SERVER_COUNT;s++) free(bufs[s]);

            // 클라이언트 처리 요약 출력
            printf("[CLIENT %d] gather=%.6f s, send=%.6f s, ints_sent=%d\n",
                   c,gather_time,send_time,end-start);
            fflush(stdout);
            exit(0);
        }
    }

    // 4. 모든 클라이언트 종료 대기
    for(int c=0;c<SM_COUNT;c++) wait(NULL);

    // 5. 서버 종료 신호 전송 (count=0)
    // 서버는 count=0 메시지를 받으면 루프 종료
    for(int s=0;s<SERVER_COUNT;s++){
        struct msgbuf msg={1,0,{0}};
        msgsnd(qids[s],&msg,sizeof(msg.count),0);
    }

    // 6. 서버 종료 대기
    for(int s=0;s<SERVER_COUNT;s++) wait(NULL);

    // 7. 메시지 큐 제거
    for(int s=0;s<SERVER_COUNT;s++) msgctl(qids[s],IPC_RMID,NULL);

    return 0;
}
