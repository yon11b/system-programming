#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/sem.h>

/* ===================== MODE ===================== */
#if defined(GRID_8x8)
    #define NUM_SM 8
    #define CLIENT_CNT 8
#elif defined(GRID_4x4)
    #define NUM_SM 4
    #define LOGICAL_SM 8
    #define CLIENT_CNT 8

#else
#error "Define GRID_8x8 or GRID_4x4"
#endif
#define N 64
#define DATA_SIZE (N*N)
#define SM_CHUNK (DATA_SIZE / NUM_SM)
#define LOGICAL_CHUNK (DATA_SIZE / LOGICAL_SM)
#define CHUNK_INT 256

#define MSG_KEY 0x1234

/* ===================== MSG ===================== */
struct msgbuf {
    long mtype;
    int data[CHUNK_INT];
};

/* ===================== TIME ===================== */
#define GET_DURATION(s,e) \
 ((e.tv_sec - s.tv_sec) + (e.tv_usec - s.tv_usec)/1000000.0)

/* ===================== SEM ===================== */
union semun { int val; };

void sem_lock(int id){
    struct sembuf p={0,-1,SEM_UNDO};
    semop(id,&p,1);
}
void sem_unlock(int id){
    struct sembuf v={0,1,SEM_UNDO};
    semop(id,&v,1);
}

/* ===================== SERVER ===================== */
void server_run() {
    int msqid = msgget(MSG_KEY, IPC_CREAT | 0666);
    struct msgbuf msg;

    FILE* raid[4];
    char fn[32];

    for (int i = 0; i < 4; i++) {
        sprintf(fn, "raid_disk%d.bin", i);
        raid[i] = fopen(fn, "wb");
    }

    int total_msgs;

#if defined(GRID_8x8)
    total_msgs = NUM_SM * 2;        // 8 * 2 = 16
#elif defined(GRID_4x4)
    total_msgs = LOGICAL_SM * 2;    // 8 * 2 = 16
#endif

    struct timeval io_s, io_e;
    double io_time = 0;
   

    for (int i = 0; i < total_msgs; i++) {
        //gettimeofday(&c2s_s, NULL);
        msgrcv(msqid, &msg, sizeof(msg.data), 0, 0);
        // gettimeofday(&c2s_e, NULL);
        // c2s_time += GET_DURATION(c2s_s, c2s_e);

        int sm = msg.mtype - 1;
        int disk = sm % 4;

        gettimeofday(&io_s, NULL);
        fwrite(msg.data, sizeof(int), CHUNK_INT, raid[disk]);                
        gettimeofday(&io_e, NULL);
        fflush(raid[disk]);
        io_time += GET_DURATION(io_s, io_e);
    }
    for(int i=0;i<CLIENT_CNT;i++) wait(NULL);

    wait(NULL);  // server 종료 대기
    //printf("\n[SERVER] client->server time = %.6f sec\n", c2s_time);
    printf("[SERVER] IO time = %.6f sec\n", io_time);

    for (int i = 0; i < 4; i++) fclose(raid[i]);
    msgctl(msqid, IPC_RMID, NULL);
}

/* =========================================================
 * make_dist_4x4
 *  - logical_sm : 0 ~ 7
 *  - out        : 512 ints
 *
 * 4x4 분산 구조 (PPT 그대로)
 * ========================================================= */
void make_dist_4x4(int logical_sm, int *out)
{
    const int TILE = 16;   // 16x16
    int idx = 0;

    int tile_col = logical_sm % 4;   // 0~3
    int tile_row = logical_sm / 4;   // 0 or 1 (위/아래)

    /*
     * 각 logical SM은
     * 같은 column의 tile을
     * row 방향으로 2번 반복해서 가짐
     */
    for (int repeat = 0; repeat < 2; repeat++) {

        int base_tile_row = tile_row + repeat * 2;
        int base_row = base_tile_row * TILE;

        for (int r = base_row; r < base_row + TILE; r++) {
            for (int c = tile_col * TILE;
                 c < (tile_col + 1) * TILE;
                 c++) {

                out[idx++] = r * 64 + c;
            }
        }
    }

    /* sanity check */
    if (idx != 512) {
        fprintf(stderr,
                "[ERROR] make_dist_4x4: sm=%d, idx=%d\n",
                logical_sm, idx);
        exit(1);
    }
}


void make_dist_8x8(int sm)
{
    int cols_per_sm = N / 8;          // 8
    int start_col = sm * cols_per_sm;
    int end_col   = start_col + cols_per_sm;

    int *buf = malloc(sizeof(int) * 512);
    int idx = 0;

    for (int r = 0; r < N; r++) {
        for (int c = start_col; c < end_col; c++) {
            buf[idx++] = r * N + c;
        }
    }

    char fname[32];
    sprintf(fname, "dist_sm_%d.bin", sm);
    FILE *fp = fopen(fname, "wb");
    fwrite(buf, sizeof(int), 512, fp);
    fclose(fp);
    free(buf);
}

/* ===================== MAIN ===================== */
int main(){    
    int shmid = shmget(IPC_PRIVATE,sizeof(int)*DATA_SIZE,IPC_CREAT|0666);
    int semid = semget(IPC_PRIVATE,1,IPC_CREAT|0666);
    int sem_c2s_start = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
    semctl(sem_c2s_start, 0, SETVAL, (union semun){.val = 0});

    union semun arg={.val=1};
    semctl(semid,0,SETVAL,arg);

    int* shared = shmat(shmid,NULL,0);


#if defined(GRID_8x8)

/* ===== shared memory for client↔client ===== */
int initial_shmid = shmget(IPC_PRIVATE,
                           sizeof(int) * DATA_SIZE,
                           IPC_CREAT | 0666);
int ord_shmid  = shmget(IPC_PRIVATE,
                        sizeof(int) * DATA_SIZE,
                        IPC_CREAT | 0666);

int *shm_initial = shmat(initial_shmid, NULL, 0);
int *shm_ord     = shmat(ord_shmid, NULL, 0);

/* ===== semaphore ===== */
int sem_init = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
int sem_ord  = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);

union semun sem_arg;
sem_arg.val = 0;
semctl(sem_init, 0, SETVAL, sem_arg);
semctl(sem_ord,  0, SETVAL, sem_arg);

/* =========================================================
 * STEP 1: 8 clients → dist 생성
 * ========================================================= */
for (int sm = 0; sm < NUM_SM; sm++) {
    if (fork() == 0) {

        int idx = 0;
        int cols_per_sm = 8;
        int start_col = sm * cols_per_sm;

        int dist_buf[SM_CHUNK];

        for (int r = 0; r < N; r++) {
            for (int c = start_col; c < start_col + cols_per_sm; c++) {
                dist_buf[idx++] = r * N + c;
            }
        }

        /* dist 파일 */
        char dname[32];
        sprintf(dname, "dist_sm_%d.bin", sm);
        FILE *fd = fopen(dname, "wb");
        fwrite(dist_buf, sizeof(int), SM_CHUNK, fd);
        fclose(fd);

        /* shared memory에 기록 */
        memcpy(&shm_initial[sm * SM_CHUNK],
               dist_buf,
               sizeof(int) * SM_CHUNK);

        sem_unlock(sem_init);   // dist 완료 알림
        exit(0);
    }
}

/* =========================================================
 * STEP 2: parent → 중앙 재정렬 (client↔client 통신)
 * ========================================================= */
for (int i = 0; i < NUM_SM; i++)
    sem_lock(sem_init);   // 모든 dist 완료 대기

struct timeval cc_s, cc_e;
gettimeofday(&cc_s, NULL);

int count[NUM_SM] = {0};

/* 진짜 client↔client 통신 */
for (int v = 0; v < DATA_SIZE; v++) {

    int row = v / N;
    int col = v % N;

    int src_sm  = col / 8;
    int src_idx = row * 8 + (col % 8);

    int dst_sm = v / SM_CHUNK;
    int pos    = count[dst_sm]++;

    shm_ord[dst_sm * SM_CHUNK + pos] =
        shm_initial[src_sm * SM_CHUNK + src_idx];
}

gettimeofday(&cc_e, NULL);

printf("\n[CLIENT↔CLIENT] time = %.6f sec\n",
       GET_DURATION(cc_s, cc_e));
    /* ===== server fork ===== */
    if(fork()==0){
        server_run();
        exit(0);
    }
/* =========================================================
 * STEP 3: clients → ord 수신 + server 전송
 * ========================================================= */
for (int sm = 0; sm < NUM_SM; sm++) {
    if (fork() == 0) {

        int ord_buf[SM_CHUNK];
        memcpy(ord_buf,
               &shm_ord[sm * SM_CHUNK],
               sizeof(int) * SM_CHUNK);

        /* ord 파일 */
        char oname[32];
        sprintf(oname, "ord_sm_%d.bin", sm);
        FILE *fo = fopen(oname, "wb");
        fwrite(ord_buf, sizeof(int), SM_CHUNK, fo);
        fclose(fo);
        
        /* ===== server 전송 (CLIENT → SERVER) ===== */
        int msqid = msgget(MSG_KEY, 0666);
        struct msgbuf msg;
        msg.mtype = sm + 1;

        /* 동시 출발 */
        sem_lock(sem_c2s_start);

        struct timeval cs_s, cs_e;
        gettimeofday(&cs_s, NULL);

        /* 512 ints → 256씩 2번 */
        memcpy(msg.data, &ord_buf[0], sizeof(int) * CHUNK_INT);
        msgsnd(msqid, &msg, sizeof(msg.data), 0);

        memcpy(msg.data, &ord_buf[CHUNK_INT], sizeof(int) * CHUNK_INT);
        msgsnd(msqid, &msg, sizeof(msg.data), 0);

        gettimeofday(&cs_e, NULL);

        double cs_time = GET_DURATION(cs_s, cs_e);

        /* 결과 기록 */
        FILE *ft = fopen("c2s_time.bin", "ab");
        fwrite(&cs_time, sizeof(double), 1, ft);
        fclose(ft);

        exit(0);

        }
        else {
        sem_unlock(sem_c2s_start);  // fork 직후 바로 unlock
    }
}

// /* wait */
// for (int i = 0; i < NUM_SM * 2; i++)
//     wait(NULL);

shmdt(shm_initial);
shmdt(shm_ord);
shmctl(initial_shmid, IPC_RMID, NULL);
shmctl(ord_shmid, IPC_RMID, NULL);
    semctl(sem_init, 0, IPC_RMID);
    semctl(sem_ord,  0, IPC_RMID);

#elif defined(GRID_4x4)

/* ===== shared memory for client↔client ===== */
int initial_shmid = shmget(IPC_PRIVATE,
                           sizeof(int) * DATA_SIZE,
                           IPC_CREAT | 0666);
int ord_shmid  = shmget(IPC_PRIVATE,
                           sizeof(int) * DATA_SIZE,
                           IPC_CREAT | 0666);

int *shm_initial = shmat(initial_shmid, NULL, 0);
int *shm_ord  = shmat(ord_shmid, NULL, 0);

/* ===== semaphore ===== */
int sem_init   = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
int sem_ord = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);

union semun sem_arg;
sem_arg.val = 0;
semctl(sem_init,   0, SETVAL, sem_arg);   // init counter
semctl(sem_ord, 0, SETVAL, sem_arg);   // release counter

/* ===== 8 logical clients ===== */
for (int l = 0; l < LOGICAL_SM; l++) {
    if (fork() == 0) {

        /* ===== STEP 1: dist 생성 ===== */
        int dist_buf[LOGICAL_CHUNK];
        make_dist_4x4(l, dist_buf);   // ★ 반드시 buffer 전달

        /* dist 파일 */
        char dname[32];
        sprintf(dname, "dist_sm_%d.bin", l);
        FILE *fd = fopen(dname, "wb");
        fwrite(dist_buf, sizeof(int), LOGICAL_CHUNK, fd);
        fclose(fd);

        /* shm_initial 기록 */
        memcpy(&shm_initial[l * LOGICAL_CHUNK],
               dist_buf,
               sizeof(int) * LOGICAL_CHUNK);

        sem_unlock(sem_init);   // dist 완료 알림

        /* ===== STEP 2: ord 대기 ===== */
        sem_lock(sem_ord);
        int ord_buf[LOGICAL_CHUNK];
        memcpy(ord_buf,
               &shm_ord[l * LOGICAL_CHUNK],
               sizeof(int) * LOGICAL_CHUNK);
        /* ord 파일 */
        char oname[32];
        sprintf(oname, "ord_sm_%d.bin", l);
        FILE *fo = fopen(oname, "wb");
        fwrite(ord_buf, sizeof(int), LOGICAL_CHUNK, fo);
        fclose(fo);

        /* ===== server 전송 (CLIENT → SERVER) ===== */
        int msqid = msgget(MSG_KEY, 0666);
        struct msgbuf msg;
        msg.mtype = l + 1;

        /* 동시 출발 */
        sem_lock(sem_c2s_start);

        struct timeval cs_s, cs_e;
        gettimeofday(&cs_s, NULL);

        /* 512 ints → 256씩 2번 */
        memcpy(msg.data, &ord_buf[0], sizeof(int) * CHUNK_INT);
        msgsnd(msqid, &msg, sizeof(msg.data), 0);

        memcpy(msg.data, &ord_buf[CHUNK_INT], sizeof(int) * CHUNK_INT);
        msgsnd(msqid, &msg, sizeof(msg.data), 0);

        gettimeofday(&cs_e, NULL);

        double cs_time = GET_DURATION(cs_s, cs_e);

        /* 결과 기록 */
        FILE *ft = fopen("c2s_time.bin", "ab");
        fwrite(&cs_time, sizeof(double), 1, ft);
        fclose(ft);

        exit(0);

    }else {
        sem_unlock(sem_c2s_start);  // fork 직후 바로 unlock
    }
}

/* ===== parent: client↔client redistribution ===== */
for (int i = 0; i < LOGICAL_SM; i++)
    sem_lock(sem_init);   // 모든 dist 완료 대기
struct timeval cc_s,cc_e;
gettimeofday(&cc_s,NULL);

int temp[LOGICAL_SM][LOGICAL_CHUNK];
int count[LOGICAL_SM] = {0};

/* 중앙 재정렬 (진짜 client↔client 통신) */
for (int i = 0; i < DATA_SIZE; i++) {
    int v = shm_initial[i];
    int owner = v / LOGICAL_CHUNK;
    int pos   = count[owner]++;
    temp[owner][pos] = v;
}

for (int l = 0; l < LOGICAL_SM; l++) {
    memcpy(&shm_ord[l * LOGICAL_CHUNK],
           temp[l],
           sizeof(int) * LOGICAL_CHUNK);
    sem_unlock(sem_ord);
}
gettimeofday(&cc_e, NULL);

printf("\n[CLIENT↔CLIENT] time = %.6f sec\n",
       GET_DURATION(cc_s, cc_e));
    /* ===== server fork ===== */
    if(fork()==0){
        server_run();
        exit(0);
    }
       /* ===== wait ===== */
// for (int i = 0; i < LOGICAL_SM; i++)
//     wait(NULL);

shmdt(shm_initial);
shmdt(shm_ord);
shmctl(initial_shmid, IPC_RMID, NULL);
shmctl(ord_shmid, IPC_RMID, NULL);
    semctl(sem_init, 0, IPC_RMID);
    semctl(sem_ord,  0, IPC_RMID);

#endif

    /* ===== client → server 전송 시작 ===== */
    // for (int i = 0; i < CLIENT_CNT; i++)
    //     sem_unlock(sem_c2s_start);

    /* 모든 client 종료 대기 */
    for (int i = 0; i < CLIENT_CNT; i++)
        wait(NULL);

    /* server 종료 대기 */
    wait(NULL);

    /* client→server 시간 합산 */
    FILE *ft = fopen("c2s_time.bin", "rb");
    double sum = 0.0, t;

    while (fread(&t, sizeof(double), 1, ft) == 1)
        sum += t;

    fclose(ft);
    remove("c2s_time.bin");
    for(int i=0;i<CLIENT_CNT;i++) wait(NULL);

    wait(NULL);  // server 종료 대기
    printf("[CLIENT->SERVER] time = %.6f sec\n", sum);

    shmdt(shared);
    shmctl(shmid,IPC_RMID,NULL);
    semctl(semid,0,IPC_RMID);

    return 0;
}
