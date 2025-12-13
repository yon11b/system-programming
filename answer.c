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
#elif defined(GRID_4x4)
#define NUM_SM 4
#define LOGICAL_SM 8
#define LOGICAL_CHUNK (DATA_SIZE / LOGICAL_SM)   // 512

#else
#error "Define GRID_8x8 or GRID_4x4"
#endif

#define N 64
#define DATA_SIZE (N*N)
#define SM_CHUNK (DATA_SIZE / NUM_SM)
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

    struct timeval c2s_s, c2s_e, io_s, io_e;
    double c2s_time = 0, io_time = 0;

    for (int i = 0; i < total_msgs; i++) {
        gettimeofday(&c2s_s, NULL);
        msgrcv(msqid, &msg, sizeof(msg.data), 0, 0);
        gettimeofday(&c2s_e, NULL);
        c2s_time += GET_DURATION(c2s_s, c2s_e);

        int sm = msg.mtype - 1;
        int disk = sm % 4;

        gettimeofday(&io_s, NULL);
        fwrite(msg.data, sizeof(int), CHUNK_INT, raid[disk]);
        fflush(raid[disk]);
        gettimeofday(&io_e, NULL);
        io_time += GET_DURATION(io_s, io_e);
    }

    printf("\n[SERVER] client->server time = %.6f sec\n", c2s_time);
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
    union semun arg={.val=1};
    semctl(semid,0,SETVAL,arg);

    int* shared = shmat(shmid,NULL,0);

    /* ===== server fork ===== */
    if(fork()==0){
        server_run();
        exit(0);
    }

    struct timeval cc_s,cc_e;
    gettimeofday(&cc_s,NULL);

#if defined(GRID_8x8)
    /* ===== 8x8 : 분산 재정렬 ===== */
    for(int sm=0;sm<NUM_SM;sm++){
        if(fork()==0){
            make_dist_8x8(sm);

    // char dist_name[32];
    // sprintf(dist_name, "dist_sm_%d.bin", sm);
    // FILE *fpd = fopen(dist_name, "wb");
    // fwrite(dist_buf, sizeof(int), dist_cnt, fpd);
    // fclose(fpd);
    // free(dist_buf);

    /* ===== [STEP 2] 기존 ord + server 전송 ===== */
   

            int start=sm*SM_CHUNK;
            int* buf=malloc(sizeof(int)*SM_CHUNK);

            sem_lock(semid);
            for(int i=0;i<SM_CHUNK;i++){
                buf[i]=start+i;
                shared[start+i]=start+i;
            }
            /* ===== client 결과 파일 생성 ===== */
            char fname[32];
            sprintf(fname, "ord_sm_%d.bin", sm);
            FILE *fp = fopen(fname, "wb");
            if(fp){
                fwrite(buf, sizeof(int), SM_CHUNK, fp);
                fclose(fp);
            }
                
            sem_unlock(semid);

            int msqid=msgget(MSG_KEY,0666);
            struct msgbuf msg;
            msg.mtype=sm+1;

            memcpy(msg.data,buf,sizeof(int)*CHUNK_INT);
            msgsnd(msqid,&msg,sizeof(msg.data),0);
            memcpy(msg.data,&buf[CHUNK_INT],sizeof(int)*CHUNK_INT);
            msgsnd(msqid,&msg,sizeof(msg.data),0);

            free(buf);
            exit(0);
        }
    }
#elif defined(GRID_4x4)

/* ===== shared memory for client↔client ===== */
int initial_shmid = shmget(IPC_PRIVATE,
                           sizeof(int) * DATA_SIZE,
                           IPC_CREAT | 0666);
int domain_shmid  = shmget(IPC_PRIVATE,
                           sizeof(int) * DATA_SIZE,
                           IPC_CREAT | 0666);

int *shm_initial = shmat(initial_shmid, NULL, 0);
int *shm_domain  = shmat(domain_shmid, NULL, 0);

/* ===== semaphore ===== */
int sem_init   = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
int sem_domain = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);

union semun sem_arg;
sem_arg.val = 0;
semctl(sem_init,   0, SETVAL, sem_arg);   // init counter
semctl(sem_domain, 0, SETVAL, sem_arg);   // release counter

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

        /* ===== STEP 2: domain 대기 ===== */
        sem_lock(sem_domain);

        int ord_buf[LOGICAL_CHUNK];
        memcpy(ord_buf,
               &shm_domain[l * LOGICAL_CHUNK],
               sizeof(int) * LOGICAL_CHUNK);

        /* ord 파일 */
        char oname[32];
        sprintf(oname, "ord_sm_%d.bin", l);
        FILE *fo = fopen(oname, "wb");
        fwrite(ord_buf, sizeof(int), LOGICAL_CHUNK, fo);
        fclose(fo);

        /* ===== server 전송 ===== */
        int msqid = msgget(MSG_KEY, 0666);
        struct msgbuf msg;
        msg.mtype = l + 1;

        memcpy(msg.data, &ord_buf[0], sizeof(int) * CHUNK_INT);
        msgsnd(msqid, &msg, sizeof(msg.data), 0);

        memcpy(msg.data, &ord_buf[CHUNK_INT], sizeof(int) * CHUNK_INT);
        msgsnd(msqid, &msg, sizeof(msg.data), 0);

        exit(0);
    }
}

/* ===== parent: client↔client redistribution ===== */
for (int i = 0; i < LOGICAL_SM; i++)
    sem_lock(sem_init);   // 모든 dist 완료 대기

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
    memcpy(&shm_domain[l * LOGICAL_CHUNK],
           temp[l],
           sizeof(int) * LOGICAL_CHUNK);
    sem_unlock(sem_domain);
}

/* ===== wait ===== */
for (int i = 0; i < LOGICAL_SM; i++)
    wait(NULL);

shmdt(shm_initial);
shmdt(shm_domain);
shmctl(initial_shmid, IPC_RMID, NULL);
shmctl(domain_shmid, IPC_RMID, NULL);

#endif


    for(int i=0;i<NUM_SM;i++) wait(NULL);
    gettimeofday(&cc_e,NULL);

    printf("\n[CLIENT↔CLIENT] time = %.6f sec\n",
           GET_DURATION(cc_s,cc_e));

    wait(NULL); /* server */

    shmdt(shared);
    shmctl(shmid,IPC_RMID,NULL);
    semctl(semid,0,IPC_RMID);

    return 0;
}
