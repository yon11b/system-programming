#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/sem.h>   /* 세마포어 */
#include <unistd.h>
#include <sys/time.h>  /* 시간 측정 */

#define N 64
#define NUM_SM 8
#define DATA_SIZE (N * N)

/* 시간 차이를 초(double) 단위로 반환하는 매크로 */
#define GET_DURATION(start, end) \
    ((end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0)

/* SysV 세마포어용 공용체 정의 */
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

/* P 연산 (lock) */
void sem_lock(int semid) {
    struct sembuf p;
    p.sem_num = 0;
    p.sem_op  = -1;     /* 자원 1개 획득 */
    p.sem_flg = SEM_UNDO;
    if (semop(semid, &p, 1) == -1) {
        perror("semop P");
        exit(1);
    }
}

/* V 연산 (unlock) */
void sem_unlock(int semid) {
    struct sembuf v;
    v.sem_num = 0;
    v.sem_op  = 1;      /* 자원 1개 반환 */
    v.sem_flg = SEM_UNDO;
    if (semop(semid, &v, 1) == -1) {
        perror("semop V");
        exit(1);
    }
}

int main() {
    key_t shm_key = 946;
    key_t sem_key = 946;
    int shmid;
    int semid;
    int *shared;
    int (*matrix)[N];
    int i, j, sm;
    pid_t pid;
    FILE *fp;
    struct timeval main_start, main_end;
    union semun arg;

    /* 1. Shared Memory 생성 */
    shmid = shmget(shm_key, sizeof(int) * DATA_SIZE, IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("shmget failed");
        exit(1);
    }
    shared = (int *)shmat(shmid, NULL, 0);
    if (shared == (void *)-1) {
        perror("shmat failed");
        exit(1);
    }

    /* 2. 세마포어 생성 & 초기화 (value = 1) */
    semid = semget(sem_key, 1, IPC_CREAT | 0666);
    if (semid == -1) {
        perror("semget failed");
        exit(1);
    }
    arg.val = 1;
    if (semctl(semid, 0, SETVAL, arg) == -1) {
        perror("semctl SETVAL failed");
        exit(1);
    }

    /* 3. 초기 데이터 생성 (Input Matrix) */
    matrix = (int (*)[N])malloc(sizeof(int) * DATA_SIZE);
    if (matrix == NULL) {
        perror("malloc failed");
        exit(1);
    }
    for (i = 0; i < N; i++) {
        for (j = 0; j < N; j++) {
            matrix[i][j] = i * N + j;
        }
    }

    printf("=== 8x8 Partitioning Simulation Start (SharedMem + Semaphore) ===\n");

    gettimeofday(&main_start, NULL);  /* 전체 작업 시간 측정 시작 */

    /* 4. 프로세스 분기 (Client SM 생성) */
    for (sm = 0; sm < NUM_SM; sm++) {
        pid = fork();

        if (pid == 0) {
            /* -------- [Child Process: Client SM] -------- */
            int start_col, end_col;
            int r, c;
            int local_count;
            int *client_buffer;
            int buffer_idx = 0;
            char filename[30];
            FILE *fp_client;
            struct timeval client_start, client_end;
            double client_time;

            start_col = sm * (N / NUM_SM);       /* 예: 0,8,16,...,56 */
            end_col   = start_col + (N / NUM_SM);
            local_count = N * (N / NUM_SM);      /* 64 * 8 = 512 */

            client_buffer = (int *)malloc(sizeof(int) * local_count);
            if (client_buffer == NULL) {
                perror("malloc client_buffer");
                exit(1);
            }

            gettimeofday(&client_start, NULL);

            /* ★ 공유 메모리에 쓸 때 세마포어로 보호
               -> 한 SM이 자기 구역(512개)을 한 번에 쓰도록 lock */
            sem_lock(semid);

            for (r = 0; r < N; r++) {
                for (c = start_col; c < end_col; c++) {
                    int value = matrix[r][c];
                    client_buffer[buffer_idx++] = value;

                    /* Server(Shared Memory)에 기록 */
                    shared[r * N + c] = value;
                }
            }

            sem_unlock(semid);

            gettimeofday(&client_end, NULL);
            client_time = GET_DURATION(client_start, client_end);

            /* SM별 분산 결과 dump (od로 확인 용도) */
            sprintf(filename, "client_%d.bin", sm);
            fp_client = fopen(filename, "wb");
            if (fp_client != NULL) {
                fwrite(client_buffer, sizeof(int), local_count, fp_client);
                fclose(fp_client);
            }

            printf("[Client SM %d] Process Time: %.6f sec | Dump: %s\n",
                   sm, client_time, filename);

            free(client_buffer);
            free(matrix);
            shmdt(shared);
            /* 자식은 세마포어 삭제(X) -> 부모가 끝에서 삭제 */
            exit(0);
        }
    }

    /* 5. 부모: 자식들 종료 대기 */
    for (i = 0; i < NUM_SM; i++) {
        wait(NULL);
    }

    gettimeofday(&main_end, NULL);
    printf("=== All Clients Finished ===\n");
    printf("Total Elapsed Time: %.6f sec\n",
           GET_DURATION(main_start, main_end));

    /* 6. 최종 결과 저장 (server 역할) */
    fp = fopen("server_result_combined.bin", "wb");
    if (fp != NULL) {
        fwrite(shared, sizeof(int), DATA_SIZE, fp);
        fclose(fp);
        printf("Server Result Saved: server_result_combined.bin\n");
    }

    /* 7. IPC 자원 해제 */
    shmdt(shared);
    shmctl(shmid, IPC_RMID, NULL);
    semctl(semid, 0, IPC_RMID);   /* 세마포어 제거 */
    free(matrix);

    return 0;
}
