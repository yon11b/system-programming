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
    if (semop(id,&p,1) == -1) {
        perror("semop lock");
        exit(1);
    }
}
void sem_unlock(int id){
    struct sembuf v={0,1,SEM_UNDO};
    if (semop(id,&v,1) == -1) {
        perror("semop unlock");
        exit(1);
    }
}

// /* ===================== SERVER ===================== */
// void server_run() {
//     int msqid = msgget(MSG_KEY, IPC_CREAT | 0666);
//     if (msqid == -1) {
//         perror("msgget(server)");
//         exit(1);
//     }

//     struct msgbuf msg;

//     FILE* raid[4];
//     char fn[32];

//     for (int i = 0; i < 4; i++) {
//         sprintf(fn, "raid_disk%d.bin", i);
//         raid[i] = fopen(fn, "wb");
//         if (!raid[i]) {
//             perror("fopen raid_disk");
//             exit(1);
//         }
//     }

//     int total_msgs;

// #if defined(GRID_8x8)
//     total_msgs = NUM_SM * 2;        // 8 * 2 = 16
// #elif defined(GRID_4x4)
//     total_msgs = LOGICAL_SM * 2;    // 8 * 2 = 16
// #endif
    
//     /* 3. 자식 프로세스 4개 생성 (디스크별) */
//     pid_t disk_pid[4];
//     for (int d = 0; d < 4; d++) {
//         disk_pid[d] = fork();
//         if (disk_pid[d] == 0) {  // 자식 프로세스
//             /* 각자 자신의 파일 열기 (append 모드) */
//             char fn[32];
//             sprintf(fn, "raid_disk%d.bin", d);
//             FILE *fp = fopen(fn, "ab");
//             if (!fp) { perror("fopen child"); exit(1); }

//             struct msgbuf msg;
//             struct timeval io_s, io_e;
//             double c2s_time = 0;                
//             printf("[DEBUG] Disk %d waiting for mtype %d\n", d, d+1);


//             while (1) {
//                 /* 메시지 수신 (mtype = d+1) */
//                 struct timeval recv_s, recv_e;
//                 gettimeofday(&recv_s, NULL);
//                 int ret = msgrcv(msqid, &msg, sizeof(msg.data), d + 1, 0);

//                 gettimeofday(&recv_e, NULL);
//                 if (ret == -1) {
//                     perror("msgrcv child");
//                     break;
//                 }
//                 c2s_time += GET_DURATION(recv_s, recv_e);
//                 printf("[Disk %d] client->server time %.6f sec\n", d, c2s_time);

//                 /* 파일 쓰기 */
//                 gettimeofday(&io_s, NULL);
//                 fwrite(msg.data, sizeof(int), CHUNK_INT, fp);
//                 fflush(fp);
//                 gettimeofday(&io_e, NULL);
//                 printf("[Disk %d] wrote chunk in %.6f sec\n", d, GET_DURATION(io_s, io_e));
//             }

//             fclose(fp);
//             exit(0);
//         }
//     }
//     msgctl(msqid, IPC_RMID, NULL);
// }
/* ===================== SERVER ===================== */
void server_run() {
    int msqid = msgget(MSG_KEY, IPC_CREAT | 0666);
    if (msqid == -1) { perror("msgget"); exit(1); }

    pid_t disk_pid[4];
    int msgs_per_disk = 4;

    for (int d = 0; d < 4; d++) {
        disk_pid[d] = fork();
        if (disk_pid[d] == 0) {
            char fn[32];
            sprintf(fn, "raid_disk%d.bin", d);
            FILE *fp = fopen(fn, "wb");
            if (!fp) { perror("fopen"); exit(1); }

            struct msgbuf msg;
            struct timeval recv_s, recv_e, io_s, io_e;
            double total_c2s = 0, total_io = 0;
            for (int i = 0; i < msgs_per_disk; i++) {
                gettimeofday(&recv_s, NULL);
                if (msgrcv(msqid, &msg, sizeof(msg.data), d+1, 0) == -1) {
                    perror("msgrcv");
                    break;
                }
                gettimeofday(&recv_e, NULL);
                
                double c2s_time = GET_DURATION(recv_s, recv_e);
                total_c2s += c2s_time;
                printf("[Disk %d] client->server time %.6f sec\n", d, c2s_time);
                gettimeofday(&io_s, NULL);
                fwrite(msg.data, sizeof(int), CHUNK_INT, fp);
                fflush(fp);
                gettimeofday(&io_e, NULL);
                
                double io_time = GET_DURATION(io_s, io_e);
                total_io += io_time;
                printf("[Disk %d] wrote chunk in %.6f sec\n", d, io_time);
            }
            printf("[Disk %d] total client->server time: %.6f sec, total io time: %.6f sec\n",
                   d, total_c2s, total_io);
            fclose(fp);
            exit(0);
        }
    }

    for (int i = 0; i < 4; i++) wait(NULL);

    // 모든 child 종료 후 msg queue 제거
    if (msgctl(msqid, IPC_RMID, NULL) == -1) {
        perror("msgctl");
    }
} // <- server_run 끝


/* =========================================================
 * make_dist_4x4
 *  - logical_sm : 0 ~ 7
 *  - out        : 512 ints
 * ========================================================= */
void make_dist_4x4(int logical_sm, int *out)
{
    const int TILE = 16;   // 16x16
    int idx = 0;

    int tile_col = logical_sm % 4;   // 0~3
    int tile_row = logical_sm / 4;   // 0 or 1

    for (int repeat = 0; repeat < 2; repeat++) {
        int base_tile_row = tile_row + repeat * 2;
        int base_row = base_tile_row * TILE;

        for (int r = base_row; r < base_row + TILE; r++) {
            for (int c = tile_col * TILE; c < (tile_col + 1) * TILE; c++) {
                out[idx++] = r * 64 + c;
            }
        }
    }

    if (idx != 512) {
        fprintf(stderr, "[ERROR] make_dist_4x4: sm=%d, idx=%d\n",
                logical_sm, idx);
        exit(1);
    }
}

/* =========================================================
 * make_dist_8x8 (PPT 기준: 열 striping)
 *  - dist_sm_i.bin 저장(덤프용)
 *  - shared[sm*512..]에 dist를 기록 (client-client 통신용)
 * ========================================================= */
void make_dist_8x8(int sm, int *shared, int semid)
{
    int cols_per_sm = N / 8;          // 8
    int start_col = sm * cols_per_sm;
    int end_col   = start_col + cols_per_sm;

    int buf[512];
    int idx = 0;

    for (int r = 0; r < N; r++) {
        for (int c = start_col; c < end_col; c++) {
            buf[idx++] = r * N + c;  // global index
        }
    }

    /* dist 파일 저장 (보여주기용) */
    char fname[32];
    sprintf(fname, "dist_sm_%d.bin", sm);
    FILE *fp = fopen(fname, "wb");
    if (!fp) { perror("fopen dist"); exit(1); }
    fwrite(buf, sizeof(int), 512, fp);
    fclose(fp);

    /* shared memory에 dist 저장 (통신용) */
    /* 구역이 겹치지 않지만, 과제 요구상 semaphore 사용 가능 */
    sem_lock(semid);
    memcpy(&shared[sm * 512], buf, sizeof(int) * 512);
    sem_unlock(semid);
}

/* ===================== MAIN ===================== */
int main(){
    int shmid = shmget(IPC_PRIVATE,sizeof(int)*DATA_SIZE,IPC_CREAT|0666);
    if (shmid == -1) { perror("shmget"); exit(1); }

    int semid = semget(IPC_PRIVATE,1,IPC_CREAT|0666);
    if (semid == -1) { perror("semget"); exit(1); }

    union semun arg;
    arg.val = 1;
    if (semctl(semid,0,SETVAL,arg) == -1) { perror("semctl"); exit(1); }

    int* shared = shmat(shmid,NULL,0);
    if (shared == (void*)-1) { perror("shmat"); exit(1); }

    /* ===== server fork ===== */
    if(fork()==0){
        server_run(semid);
        exit(0);
    }

    struct timeval cc_s,cc_e;
    gettimeofday(&cc_s,NULL);

#if defined(GRID_8x8)

    /* =====================================================
     * GRID_8x8 정답 흐름
     *  Phase1: dist 생성 (shared에 dist 기록)
     *  Phase2: shared(dist) 읽어서 ord 재정렬 + server 전송
     * ===================================================== */

    /* ---------- Phase 1: dist 생성 ---------- */
    printf("=== [GRID_8x8] PHASE 1: dist_sm_i.bin 생성 + shared 저장 ===\n");
    for(int sm=0; sm<NUM_SM; sm++){
        if(fork()==0){
            make_dist_8x8(sm, shared, semid);
            printf("[PH1][SM %d] dist_sm_%d.bin 생성 완료\n", sm, sm);
            exit(0);
        }
    }
    for(int i=0;i<NUM_SM;i++) wait(NULL);

    /* ---------- Phase 2: ord 재정렬 + server 전송 ---------- */
    printf("\n=== [GRID_8x8] PHASE 2: shared(dist) 기반 ord 재정렬 + server 전송 ===\n");
    for(int sm=0; sm<NUM_SM; sm++){
        if(fork()==0){
            int start = sm * SM_CHUNK;            // 0,512,1024,...
            int *ord_buf = (int*)malloc(sizeof(int)*SM_CHUNK);
            if (!ord_buf) { perror("malloc ord_buf"); exit(1); }

            /* STEP2: 전역 순서(start~start+511)를 만들되,
               값은 "shared의 dist 결과"에서 읽어서 채운다 */
            for(int i=0; i<SM_CHUNK; i++){
                int global = start + i;           // 우리가 만들어야 하는 global index

                int row = global / N;
                int col = global % N;

                int owner_sm  = col / 8;          // dist가 들어있는 SM
                int owner_pos = row * 8 + (col % 8); // dist 내부 위치 (0~511)

                ord_buf[i] = shared[owner_sm * 512 + owner_pos];
            }

            /* ord 파일 저장 (보여주기용) */
            char fname[32];
            sprintf(fname, "ord_sm_%d.bin", sm);
            FILE *fp = fopen(fname, "wb");
            if(fp){
                fwrite(ord_buf, sizeof(int), SM_CHUNK, fp);
                fclose(fp);
            } else {
                perror("fopen ord");
                exit(1);
            }

            /* server 전송: 256 ints * 2 chunks */
            //int msqid = msgget(MSG_KEY, 0666);
            int msqid = -1;
            while ((msqid = msgget(MSG_KEY, 0666)) == -1) {
                usleep(1000); // 1ms 대기
            }
            if (msqid == -1) { perror("msgget(client)"); exit(1); }

            struct msgbuf msg;
            // msg.mtype = sm + 1;
            msg.mtype = (sm % 4) + 1;

            memcpy(msg.data, &ord_buf[0], sizeof(int)*CHUNK_INT);
            if (msgsnd(msqid, &msg, sizeof(msg.data), 0) == -1) {
                perror("msgsnd chunk0");
                exit(1);
            }

            memcpy(msg.data, &ord_buf[CHUNK_INT], sizeof(int)*CHUNK_INT);
            if (msgsnd(msqid, &msg, sizeof(msg.data), 0) == -1) {
                perror("msgsnd chunk1");
                exit(1);
            }

            printf("[PH2][SM %d] ord_sm_%d.bin 생성 + server 전송 완료 (global %d~%d)\n",
                   sm, sm, start, start + SM_CHUNK - 1);

            free(ord_buf);
            exit(0);
        }
    }

    /* Phase2 자식 대기 */
    for(int i=0;i<NUM_SM;i++) wait(NULL);

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
    semctl(sem_init,   0, SETVAL, sem_arg);
    semctl(sem_domain, 0, SETVAL, sem_arg);

    /* ===== 8 logical clients ===== */
    for (int l = 0; l < LOGICAL_SM; l++) {
        if (fork() == 0) {

            /* ===== STEP 1: dist 생성 ===== */
            int dist_buf[LOGICAL_CHUNK];
            make_dist_4x4(l, dist_buf);

            char dname[32];
            sprintf(dname, "dist_sm_%d.bin", l);
            FILE *fd = fopen(dname, "wb");
            fwrite(dist_buf, sizeof(int), LOGICAL_CHUNK, fd);
            fclose(fd);

            memcpy(&shm_initial[l * LOGICAL_CHUNK],
                   dist_buf,
                   sizeof(int) * LOGICAL_CHUNK);

            sem_unlock(sem_init);

            /* ===== STEP 2: domain 대기 ===== */
            sem_lock(sem_domain);

            int ord_buf[LOGICAL_CHUNK];
            memcpy(ord_buf,
                   &shm_domain[l * LOGICAL_CHUNK],
                   sizeof(int) * LOGICAL_CHUNK);

            char oname[32];
            sprintf(oname, "ord_sm_%d.bin", l);
            FILE *fo = fopen(oname, "wb");
            fwrite(ord_buf, sizeof(int), LOGICAL_CHUNK, fo);
            fclose(fo);

            // int msqid = msgget(MSG_KEY, 0666);
            int msqid = -1;
            while ((msqid = msgget(MSG_KEY, 0666)) == -1) {
                usleep(1000); // 1ms 대기
            }
            struct msgbuf msg;
            // msg.mtype = l + 1;
            msg.mtype = (l % 4) + 1;

            memcpy(msg.data, &ord_buf[0], sizeof(int) * CHUNK_INT);
            msgsnd(msqid, &msg, sizeof(msg.data), 0);

            memcpy(msg.data, &ord_buf[CHUNK_INT], sizeof(int) * CHUNK_INT);
            msgsnd(msqid, &msg, sizeof(msg.data), 0);

            exit(0);
        }
    }

    /* ===== parent: client↔client redistribution ===== */
    for (int i = 0; i < LOGICAL_SM; i++)
        sem_lock(sem_init);

    int temp[LOGICAL_SM][LOGICAL_CHUNK];
    int count[LOGICAL_SM] = {0};

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

    for (int i = 0; i < LOGICAL_SM; i++)
        wait(NULL);

    shmdt(shm_initial);
    shmdt(shm_domain);
    shmctl(initial_shmid, IPC_RMID, NULL);
    shmctl(domain_shmid, IPC_RMID, NULL);

#endif

    gettimeofday(&cc_e,NULL);

    printf("\n[CLIENT↔CLIENT] time = %.6f sec\n",
           GET_DURATION(cc_s,cc_e));

    wait(NULL); /* server */

    shmdt(shared);
    shmctl(shmid,IPC_RMID,NULL);
    semctl(semid,0,IPC_RMID);

    return 0;
}
