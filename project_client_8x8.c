#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/sem.h>
#include <unistd.h>
#include <sys/time.h>

#define N 64
#define NUM_SM 8
#define DATA_SIZE (N * N)      /* 4096 */
#define SM_CHUNK (DATA_SIZE / NUM_SM)  /* 512 */

// server에 msgsnd 보내는 부분 추가
#include <sys/msg.h>

#define MSG_KEY 0x1234
#define CHUNK_INT 256

struct msgbuf {
    long mtype;              // client id (1~8)
    // int  chunk_idx;          // 0 or 1
    int  data[CHUNK_INT];    // 1KB
};




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
    p.sem_op  = -1;
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
    v.sem_op  = 1;
    v.sem_flg = SEM_UNDO;
    if (semop(semid, &v, 1) == -1) {
        perror("semop V");
        exit(1);
    }
}

int main() {
    key_t shm_key = 9946;
    key_t sem_key = 9947;
    int shmid;
    int semid;
    int *shared;            /* 전역 0~4095 데이터 저장용 */
    int (*matrix)[N];
    int i, j, sm;
    pid_t pid;
    struct timeval main_start, main_end;
    union semun arg;

    /* ===================== [공용 IPC 생성] ===================== */
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

    /* ===================== [원본 64x64 행렬 생성] ===================== */
    matrix = (int (*)[N])malloc(sizeof(int) * DATA_SIZE);
    if (matrix == NULL) {
        perror("malloc matrix failed");
        exit(1);
    }
    for (i = 0; i < N; i++) {
        for (j = 0; j < N; j++) {
            matrix[i][j] = i * N + j;   /* 0 ~ 4095 */
        }
    }

    printf("=== STEP 1: 8x8 Partitioning & part_sm_i.bin 생성 ===\n");
    gettimeofday(&main_start, NULL);

    /* ===================== [STEP 1: 병렬 분산 파일(part_sm_i.bin) 생성] ===================== */
    for (sm = 0; sm < NUM_SM; sm++) {
        pid = fork();
        if (pid == 0) {
            /* ----- Child: SM 프로세스 (1단계: 분산 데이터 파일 생성) ----- */
            int start_col, end_col;
            int r, c;
            int local_count;
            int *part_buffer;
            int idx;
            char filename[32];
            FILE *fp_part;
            struct timeval sm_start, sm_end;
            double sm_time;

            start_col = sm * (N / NUM_SM);       /* 0,8,16,...,56 */
            end_col   = start_col + (N / NUM_SM);/* 각각 8칸 */
            local_count = N * (N / NUM_SM);      /* 64 * 8 = 512 */

            part_buffer = (int *)malloc(sizeof(int) * local_count);
            if (part_buffer == NULL) {
                perror("malloc part_buffer failed");
                exit(1);
            }

            gettimeofday(&sm_start, NULL);

            idx = 0;
            /* 원본 행렬에서 자기 담당 구역(8열)을 그대로 flatten */
            for (r = 0; r < N; r++) {
                for (c = start_col; c < end_col; c++) {
                    part_buffer[idx++] = matrix[r][c];
                }
            }

            /* 분산 데이터 파일 저장 */
            sprintf(filename, "part_sm_%d.bin", sm);
            fp_part = fopen(filename, "wb");
            if (fp_part != NULL) {
                fwrite(part_buffer, sizeof(int), local_count, fp_part);
                fclose(fp_part);
            }

            gettimeofday(&sm_end, NULL);
            sm_time = GET_DURATION(sm_start, sm_end);

            printf("[STEP1][SM %d] part_sm_%d.bin 생성 완료 (512개) | time = %.6f sec\n",
                   sm, sm, sm_time);

            free(part_buffer);
            free(matrix);
            shmdt(shared);
            exit(0);
        }
    }

    /* 부모: STEP1 자식 종료 대기 */
    for (i = 0; i < NUM_SM; i++) {
        wait(NULL);
    }

    gettimeofday(&main_end, NULL);
    printf("=== STEP 1 완료: 모든 part_sm_i.bin 생성 ===\n");
    printf("STEP 1 Elapsed Time: %.6f sec\n\n",
           GET_DURATION(main_start, main_end));

    /* ===================== [STEP 2: 정렬된 연속 데이터 0~4095 생성 + ord_sm_i.bin 저장] ===================== */

    printf("=== STEP 2: 전역 인덱스(0~4095) 기준 재정렬 & ord_sm_i.bin 생성 ===\n");
    gettimeofday(&main_start, NULL);

    /* shared 메모리 초기화 (옵션) */
    for (i = 0; i < DATA_SIZE; i++) {
        shared[i] = -1;
    }

    for (sm = 0; sm < NUM_SM; sm++) {
        pid = fork();
        if (pid == 0) {
            /* ----- Child: SM 프로세스 (2단계: 정렬된 연속 데이터 작성) ----- */
            int k;
            int global_start;
            int *ord_buffer;
            char filename[32];
            FILE *fp_ord;
            struct timeval sm_start2, sm_end2;
            double sm_time2;
            int msqid;
            struct msgbuf msg;

            global_start = sm * SM_CHUNK;   /* 0, 512, 1024, ... */
            ord_buffer = (int *)malloc(sizeof(int) * SM_CHUNK);
            if (ord_buffer == NULL) {
                perror("malloc ord_buffer failed");
                exit(1);
            }
            msqid = msgget(MSG_KEY, IPC_CREAT | 0666);
            if (msqid == -1) {
                perror("msgget failed (client)");
                exit(1);
            }


            gettimeofday(&sm_start2, NULL);

            /* 세마포어로 보호 (실제론 구역이 겹치지 않지만, 과제 요구사항상 사용) */
            sem_lock(semid);

            for (k = 0; k < SM_CHUNK; k++) {
                int global_index = global_start + k; /* 0~511, 512~1023, ... */

                /* 전역 인덱스 기반 연속 데이터 */
                ord_buffer[k] = global_index;

                /* shared memory에도 동일하게 기록 */
                shared[global_index] = global_index;
            }

            sem_unlock(semid);

            gettimeofday(&sm_end2, NULL);
            sm_time2 = GET_DURATION(sm_start2, sm_end2);
            msg.mtype = sm + 1;

            /* ===================== 첫 번째 256개 ===================== */
            // msg.chunk_idx = 0; // 첫번째 chunk 표시
            memcpy(msg.data, &ord_buffer[0], sizeof(int) * CHUNK_INT);

            if (msgsnd(msqid, &msg, sizeof(msg.data), 0) == -1) {
                perror("msgsnd failed (1st chunk)");
                exit(1);
            }

            printf("[CLIENT][SM %d] chunk 1 전송 (global %d~%d)\n",
                sm, global_start, global_start + 255);

            /* ===================== 두 번째 256개 ===================== */            
            // msg.chunk_idx = 1; // 두번째 chunk 표시
            memcpy(msg.data, &ord_buffer[CHUNK_INT], sizeof(int) * CHUNK_INT);

            if (msgsnd(msqid, &msg, sizeof(msg.data), 0) == -1) {
                perror("msgsnd failed (2nd chunk)");
                exit(1);
            }

            printf("[CLIENT][SM %d] chunk 2 전송 (global %d~%d)\n",
                sm, global_start + 256, global_start + 511);
            /* 정렬된 연속 데이터 파일 저장 */
            sprintf(filename, "ord_sm_%d.bin", sm);
            fp_ord = fopen(filename, "wb");
            if (fp_ord != NULL) {
                fwrite(ord_buffer, sizeof(int), SM_CHUNK, fp_ord);
                fclose(fp_ord);
            }

            printf("[STEP2][SM %d] ord_sm_%d.bin 생성 완료 (global %d~%d) | time = %.6f sec\n",
                   sm, sm, global_start, global_start + SM_CHUNK - 1, sm_time2);

            free(ord_buffer);
            free(matrix);      /* parent에서 malloc 했지만 child에서도 free 가능 */
            shmdt(shared);
            exit(0);
        }
    }

    /* 부모: STEP2 자식 종료 대기 */
    for (i = 0; i < NUM_SM; i++) {
        wait(NULL);
    }

    gettimeofday(&main_end, NULL);
    printf("=== STEP 2 완료: 모든 ord_sm_i.bin 생성 ===\n");
    printf("STEP 2 Elapsed Time: %.6f sec\n",
           GET_DURATION(main_start, main_end));

    /* (옵션) shared 전체를 한 번에 확인하고 싶으면 여기서 덤프용 파일로 저장 가능
       예: global_merged.bin 으로 저장해서 0~4095가 잘 들어갔는지 확인 */

    /* IPC 자원 해제 */
    shmdt(shared);
    shmctl(shmid, IPC_RMID, NULL);
    semctl(semid, 0, IPC_RMID);
    free(matrix);

    return 0;
}