#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/time.h> /* 시간 측정을 위한 헤더 */

#define N 64
#define NUM_SM 8
#define DATA_SIZE (N * N)

/* 시간 차이를 초(double) 단위로 반환하는 매크로 */
#define GET_DURATION(start, end) \
    ((end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0)

int main() {
    /* [C90 호환] 모든 변수는 블록 최상단에 선언해야 합니다. */
    key_t shm_key = 1234;
    int shmid;
    int *shared;
    int (*matrix)[N];
    int i, j, sm;
    pid_t pid;
    FILE *fp;
    
    /* 전체 실행 시간 측정용 변수 */
    struct timeval main_start, main_end;
    
    /* 1. Shared Memory 생성 (Server 역할) */
    shmid = shmget(shm_key, sizeof(int) * DATA_SIZE, IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("shmget failed");
        exit(1);
    }
    shared = (int *)shmat(shmid, NULL, 0);

    /* 2. 초기 데이터 생성 (Input Matrix) */
    matrix = malloc(sizeof(int) * DATA_SIZE);
    for (i = 0; i < N; i++) {
        for (j = 0; j < N; j++) {
            matrix[i][j] = i * N + j;
        }
    }

    printf("=== 8x8 Partitioning Simulation Start ===\n");
    
    /* 타이머 시작 (전체 작업) */
    gettimeofday(&main_start, NULL);

    /* 3. 프로세스 분기 (Client SM 생성) */
    for (sm = 0; sm < NUM_SM; sm++) {
        pid = fork();
        
        if (pid == 0) {
            /* [Child Process: Client SM] */
            /* 자식 프로세스 내부 변수 선언 */
            int start_col, end_col;
            int r, c;
            int local_count;
            int *client_buffer;
            int buffer_idx = 0;
            char filename[30];
            FILE *fp_client;
            
            /* Client 수행 시간 측정용 변수 */
            struct timeval client_start, client_end;
            double client_time;

            start_col = sm * (N / NUM_SM);
            end_col = start_col + (N / NUM_SM);
            local_count = N * (N / NUM_SM); /* 64 * 8 = 512 int */
            
            client_buffer = (int *)malloc(sizeof(int) * local_count);

            /* Client 작업 시작 시간 기록 */
            gettimeofday(&client_start, NULL);

            /* 데이터 수집 및 전송 (Gather & Send) */
            for (r = 0; r < N; r++) {
                for (c = start_col; c < end_col; c++) {
                    int value = matrix[r][c];

                    /* 1. Local Buffer에 수집 (Client Gathering) */
                    client_buffer[buffer_idx++] = value;

                    /* 2. Server(Shared Memory)로 전송 (Client-Server Comm) */
                    /* Race Condition이 없으므로 Lock 불필요 */
                    shared[r * N + c] = value;
                }
            }

            /* Client 작업 종료 시간 기록 */
            gettimeofday(&client_end, NULL);
            client_time = GET_DURATION(client_start, client_end);

            /* 결과 파일 저장 (덤프 확인용) */
            sprintf(filename, "client_%d.bin", sm);
            fp_client = fopen(filename, "wb");
            if (fp_client) {
                fwrite(client_buffer, sizeof(int), local_count, fp_client);
                fclose(fp_client);
            }

            printf("[Client SM %d] Process Time: %.6f sec | Dump: %s\n", sm, client_time, filename);

            /* 자원 해제 및 종료 */
            free(client_buffer);
            free(matrix); 
            shmdt(shared);
            exit(0);
        }
    }

    /* 4. 부모 프로세스: 모든 자식 종료 대기 */
    for (i = 0; i < NUM_SM; i++) {
        wait(NULL);
    }

    /* 타이머 종료 (전체 작업) */
    gettimeofday(&main_end, NULL);
    printf("=== All Clients Finished ===\n");
    printf("Total Elapsed Time: %.6f sec\n", GET_DURATION(main_start, main_end));

    /* 5. 최종 결과 저장 (Server Side) */
    fp = fopen("server_result_combined.bin", "wb");
    if (fp) {
        fwrite(shared, sizeof(int), DATA_SIZE, fp);
        fclose(fp);
        printf("Server Result Saved: server_result_combined.bin\n");
    }

    /* 6. IPC 자원 해제 */
    shmdt(shared);
    shmctl(shmid, IPC_RMID, NULL);
    free(matrix);

    return 0;
}