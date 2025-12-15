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
#define LOGICAL_SM 8

#if defined(GRID_8x8)
#define NUM_SM 8
#elif defined(GRID_4x4)
#define NUM_SM 4
#define LOGICAL_CHUNK (DATA_SIZE / LOGICAL_SM)   /* 512 */
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

void sem_wait_s(int id) {
    struct sembuf p = {0, -1, 0};
    semop(id, &p, 1);
}

void sem_post_s(int id) {
    struct sembuf v = {0, 1, 0};
    semop(id, &v, 1);
}

/* ===================== SERVER ===================== */
void server_run(double *server_times) {
    int msqid;
    struct msgbuf msg;
    FILE* raid[4];
    char fn[32];
    int i;
    int total_msgs;
    struct timeval c2s_s, c2s_e, io_s, io_e;
    double c2s_time = 0, io_time = 0;
    int sm, disk;
    
    msqid = msgget(MSG_KEY, IPC_CREAT | 0666);
    if (msqid == -1) { perror("msgget(server)"); exit(1); }

    for (i = 0; i < 4; i++) {
        sprintf(fn, "raid_disk%d.bin", i);
        raid[i] = fopen(fn, "wb");
        if (!raid[i]) { perror("fopen raid_disk"); exit(1); }
    }

    total_msgs = LOGICAL_SM * 2;  /* 8 * 2 = 16 */

    for (i = 0; i < total_msgs; i++) {
        gettimeofday(&c2s_s, NULL);
        if (msgrcv(msqid, &msg, sizeof(msg.data), 0, 0) == -1) {
            perror("msgrcv"); exit(1);
        }
        gettimeofday(&c2s_e, NULL);
        c2s_time += GET_DURATION(c2s_s, c2s_e);

        sm = (int)msg.mtype - 1;
        disk = sm % 4;

        gettimeofday(&io_s, NULL);
        fwrite(msg.data, sizeof(int), CHUNK_INT, raid[disk]);
        fflush(raid[disk]);
        gettimeofday(&io_e, NULL);
        io_time += GET_DURATION(io_s, io_e);
    }

    /* Store times to shared memory */
    server_times[0] = c2s_time;
    server_times[1] = io_time;

    for (i = 0; i < 4; i++) fclose(raid[i]);
    msgctl(msqid, IPC_RMID, NULL);
}

/* ===================== DIST FUNCTIONS ===================== */
void make_dist_4x4(int logical_sm, int *out) {
    const int TILE = 16;
    int idx = 0;
    int tile_col = logical_sm % 4;
    int tile_row = logical_sm / 4;
    int repeat, base_tile_row, base_row, r, c;
    
    for (repeat = 0; repeat < 2; repeat++) {
        base_tile_row = tile_row + repeat * 2;
        base_row = base_tile_row * TILE;
        for (r = base_row; r < base_row + TILE; r++) {
            for (c = tile_col * TILE; c < (tile_col + 1) * TILE; c++) {
                out[idx++] = r * 64 + c;
            }
        }
    }
}

void make_dist_8x8(int sm, int *out) {
    int cols_per_sm = N / 8;
    int start_col = sm * cols_per_sm;
    int end_col = start_col + cols_per_sm;
    int idx = 0;
    int r, c;
    
    for (r = 0; r < N; r++) {
        for (c = start_col; c < end_col; c++) {
            out[idx++] = r * N + c;
        }
    }
}

/* ===================== MAIN ===================== */
int main() {
    int shmid, semid;
    int *shared;
    union semun arg;
    int i;
    
    /* Shared memory for server timing results */
    int server_time_shmid;
    double *server_times;
    
    /* Barrier semaphores */
    int sem_ready, sem_go_cc, sem_done_cc, sem_go_cs, sem_done_cs;
    int *counters;  /* [0]=ready, [1]=done_cc, [2]=done_cs */
    int counter_shmid;
    
    struct timeval total_cc_s, total_cc_e, total_cs_s, total_cs_e;
    
    /* Create shared memory */
    shmid = shmget(IPC_PRIVATE, sizeof(int) * DATA_SIZE, IPC_CREAT | 0666);
    shared = shmat(shmid, NULL, 0);
    
    server_time_shmid = shmget(IPC_PRIVATE, sizeof(double) * 2, IPC_CREAT | 0666);
    server_times = shmat(server_time_shmid, NULL, 0);
    
    counter_shmid = shmget(IPC_PRIVATE, sizeof(int) * 3, IPC_CREAT | 0666);
    counters = shmat(counter_shmid, NULL, 0);
    counters[0] = counters[1] = counters[2] = 0;
    
    /* Semaphores */
    semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
    sem_ready = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
    sem_go_cc = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
    sem_done_cc = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
    sem_go_cs = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
    sem_done_cs = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
    
    arg.val = 1;
    semctl(semid, 0, SETVAL, arg);
    semctl(sem_ready, 0, SETVAL, arg);
    semctl(sem_done_cc, 0, SETVAL, arg);
    semctl(sem_done_cs, 0, SETVAL, arg);
    
    arg.val = 0;
    semctl(sem_go_cc, 0, SETVAL, arg);
    semctl(sem_go_cs, 0, SETVAL, arg);
    
    /* Fork server */
    if (fork() == 0) {
        server_run(server_times);
        exit(0);
    }
    
    usleep(10000);

#if defined(GRID_8x8)
    printf("=== [GRID_8x8] 8 SM parallel execution ===\n\n");
    fflush(stdout);
    
    /* Phase 1: dist 생성 */
    for (i = 0; i < NUM_SM; i++) {
        if (fork() == 0) {
            int dist_buf[512];
            char fname[32];
            FILE *fp;
            
            make_dist_8x8(i, dist_buf);
            
            sprintf(fname, "dist_sm_%d.bin", i);
            fp = fopen(fname, "wb");
            fwrite(dist_buf, sizeof(int), 512, fp);
            fclose(fp);
            
            sem_wait_s(semid);
            memcpy(&shared[i * 512], dist_buf, sizeof(int) * 512);
            sem_post_s(semid);
            
            exit(0);
        }
    }
    for (i = 0; i < NUM_SM; i++) wait(NULL);
    printf("[Phase 1] dist 생성 완료\n\n");
    fflush(stdout);
    
    /* Phase 2 & 3: 재정렬 + 전송 */
    counters[0] = counters[1] = counters[2] = 0;
    
    for (i = 0; i < NUM_SM; i++) {
        if (fork() == 0) {
            int sm = i;
            int start = sm * SM_CHUNK;
            int ord_buf[SM_CHUNK];
            int j, global, row, col, owner_sm, owner_pos;
            char fname[32];
            FILE *fp;
            int msqid;
            struct msgbuf msg;
            
            /* Signal ready */
            sem_wait_s(sem_ready);
            counters[0]++;
            sem_post_s(sem_ready);
            
            /* Wait for GO (client-client) */
            sem_wait_s(sem_go_cc);
            
            /* Client-Client: 재정렬 */
            for (j = 0; j < SM_CHUNK; j++) {
                global = start + j;
                row = global / N;
                col = global % N;
                owner_sm = col / 8;
                owner_pos = row * 8 + (col % 8);
                ord_buf[j] = shared[owner_sm * 512 + owner_pos];
            }
            
            sprintf(fname, "ord_sm_%d.bin", sm);
            fp = fopen(fname, "wb");
            fwrite(ord_buf, sizeof(int), SM_CHUNK, fp);
            fclose(fp);
            
            /* Signal done (client-client) */
            sem_wait_s(sem_done_cc);
            counters[1]++;
            sem_post_s(sem_done_cc);
            
            /* Wait for GO (client-server) */
            sem_wait_s(sem_go_cs);
            
            /* Client-Server: msgsnd */
            msqid = msgget(MSG_KEY, 0666);
            msg.mtype = sm + 1;
            
            memcpy(msg.data, &ord_buf[0], sizeof(int) * CHUNK_INT);
            msgsnd(msqid, &msg, sizeof(msg.data), 0);
            
            memcpy(msg.data, &ord_buf[CHUNK_INT], sizeof(int) * CHUNK_INT);
            msgsnd(msqid, &msg, sizeof(msg.data), 0);
            
            /* Signal done (client-server) */
            sem_wait_s(sem_done_cs);
            counters[2]++;
            sem_post_s(sem_done_cs);
            
            exit(0);
        }
    }
    
    /* Wait for ready */
    while (1) {
        sem_wait_s(sem_ready);
        int r = counters[0];
        sem_post_s(sem_ready);
        if (r == NUM_SM) break;
        usleep(100);
    }
    
    /* Start client-client */
    gettimeofday(&total_cc_s, NULL);
    for (i = 0; i < NUM_SM; i++) sem_post_s(sem_go_cc);
    
    /* Wait for client-client done */
    while (1) {
        sem_wait_s(sem_done_cc);
        int d = counters[1];
        sem_post_s(sem_done_cc);
        if (d == NUM_SM) break;
        usleep(100);
    }
    gettimeofday(&total_cc_e, NULL);
    
    /* Start client-server */
    gettimeofday(&total_cs_s, NULL);
    for (i = 0; i < NUM_SM; i++) sem_post_s(sem_go_cs);
    
    /* Wait for client-server done */
    while (1) {
        sem_wait_s(sem_done_cs);
        int d = counters[2];
        sem_post_s(sem_done_cs);
        if (d == NUM_SM) break;
        usleep(100);
    }
    gettimeofday(&total_cs_e, NULL);
    
    for (i = 0; i < NUM_SM; i++) wait(NULL);

#elif defined(GRID_4x4)
    printf("=== [GRID_4x4] 8 logical SM parallel execution ===\n\n");
    fflush(stdout);
    
    {
    int initial_shmid, domain_shmid;
    int *shm_initial, *shm_domain;
    int sem_dist_done, sem_redist_done, sem_go_send;
    union semun sem_arg;
    int l;
    
    initial_shmid = shmget(IPC_PRIVATE, sizeof(int) * DATA_SIZE, IPC_CREAT | 0666);
    domain_shmid = shmget(IPC_PRIVATE, sizeof(int) * DATA_SIZE, IPC_CREAT | 0666);
    shm_initial = shmat(initial_shmid, NULL, 0);
    shm_domain = shmat(domain_shmid, NULL, 0);
    
    sem_dist_done = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
    sem_redist_done = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
    sem_go_send = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
    
    sem_arg.val = 0;
    semctl(sem_dist_done, 0, SETVAL, sem_arg);
    semctl(sem_redist_done, 0, SETVAL, sem_arg);
    semctl(sem_go_send, 0, SETVAL, sem_arg);
    
    counters[0] = counters[1] = counters[2] = 0;
    
    /* 8 logical clients (병렬) */
    for (l = 0; l < LOGICAL_SM; l++) {
        if (fork() == 0) {
            int dist_buf[512];
            int ord_buf[512];
            char fname[32];
            FILE *fp;
            int msqid;
            struct msgbuf msg;
            int j, target_global, src_sm, src_pos;
            
            /* Phase 1: dist 생성 */
            make_dist_4x4(l, dist_buf);
            
            sprintf(fname, "dist_sm_%d.bin", l);
            fp = fopen(fname, "wb");
            fwrite(dist_buf, sizeof(int), 512, fp);
            fclose(fp);
            
            /* shared memory에 저장 */
            sem_wait_s(semid);
            memcpy(&shm_initial[l * 512], dist_buf, sizeof(int) * 512);
            sem_post_s(semid);
            
            /* Signal dist done */
            sem_wait_s(sem_done_cc);
            counters[0]++;
            sem_post_s(sem_done_cc);
            
            /* Wait for redistribution GO signal */
            sem_wait_s(sem_redist_done);
            
            /* Phase 2: Client-Client 재정렬 (각 client가 자기 domain 데이터 수집) */
            /* 나의 domain: global index l*512 ~ (l+1)*512 - 1 */
            for (j = 0; j < 512; j++) {
                target_global = l * 512 + j;
                
                /* 이 global index가 어느 SM의 dist에 있는지 찾기 */
                /* 4x4 partitioning: 16x16 tile 기반 */
                {
                    int row = target_global / N;
                    int col = target_global % N;
                    int tile_row = row / 16;
                    int tile_col = col / 16;
                    int src_logical_sm = (tile_row % 2) * 4 + tile_col;
                    int local_row = row % 16;
                    int local_col = col % 16;
                    int repeat = tile_row / 2;
                    src_pos = repeat * 256 + local_row * 16 + local_col;
                    src_sm = src_logical_sm;
                }
                
                ord_buf[j] = shm_initial[src_sm * 512 + src_pos];
            }
            
            /* Save ord file */
            sprintf(fname, "ord_sm_%d.bin", l);
            fp = fopen(fname, "wb");
            fwrite(ord_buf, sizeof(int), 512, fp);
            fclose(fp);
            
            /* Signal redistribution done */
            sem_wait_s(sem_done_cs);
            counters[1]++;
            sem_post_s(sem_done_cs);
            
            /* Wait for GO to send */
            sem_wait_s(sem_go_send);
            
            /* Phase 3: Client-Server 전송 */
            msqid = msgget(MSG_KEY, 0666);
            msg.mtype = l + 1;
            
            memcpy(msg.data, &ord_buf[0], sizeof(int) * CHUNK_INT);
            msgsnd(msqid, &msg, sizeof(msg.data), 0);
            
            memcpy(msg.data, &ord_buf[CHUNK_INT], sizeof(int) * CHUNK_INT);
            msgsnd(msqid, &msg, sizeof(msg.data), 0);
            
            /* Signal send done */
            sem_wait_s(sem_ready);
            counters[2]++;
            sem_post_s(sem_ready);
            
            exit(0);
        }
    }
    
    /* Wait for all dist done */
    while (1) {
        sem_wait_s(sem_done_cc);
        int d = counters[0];
        sem_post_s(sem_done_cc);
        if (d == LOGICAL_SM) break;
        usleep(100);
    }
    printf("[Phase 1] dist 생성 완료\n\n");
    fflush(stdout);
    
    /* Start client-client redistribution (병렬) */
    gettimeofday(&total_cc_s, NULL);
    for (l = 0; l < LOGICAL_SM; l++) sem_post_s(sem_redist_done);
    
    /* Wait for redistribution done */
    while (1) {
        sem_wait_s(sem_done_cs);
        int d = counters[1];
        sem_post_s(sem_done_cs);
        if (d == LOGICAL_SM) break;
        usleep(100);
    }
    gettimeofday(&total_cc_e, NULL);
    
    /* Start client-server */
    gettimeofday(&total_cs_s, NULL);
    for (l = 0; l < LOGICAL_SM; l++) sem_post_s(sem_go_send);
    
    /* Wait for send done */
    while (1) {
        sem_wait_s(sem_ready);
        int d = counters[2];
        sem_post_s(sem_ready);
        if (d == LOGICAL_SM) break;
        usleep(100);
    }
    gettimeofday(&total_cs_e, NULL);
    
    for (i = 0; i < LOGICAL_SM; i++) wait(NULL);
    
    shmdt(shm_initial);
    shmdt(shm_domain);
    shmctl(initial_shmid, IPC_RMID, NULL);
    shmctl(domain_shmid, IPC_RMID, NULL);
    semctl(sem_dist_done, 0, IPC_RMID);
    semctl(sem_redist_done, 0, IPC_RMID);
    semctl(sem_go_send, 0, IPC_RMID);
    }
#endif
    
    /* Wait for server */
    wait(NULL);
    
    /* Print results */
    printf("\n========== TIMING RESULTS ==========\n");
    printf("[CLIENT-CLIENT] %.6f sec (shared memory 재정렬, 병렬)\n", 
           GET_DURATION(total_cc_s, total_cc_e));
    printf("[CLIENT-SERVER] %.6f sec (msgsnd 완료까지, 병렬)\n", 
           GET_DURATION(total_cs_s, total_cs_e));
    printf("[SERVER RECV]   %.6f sec (msgrcv 누적)\n", server_times[0]);
    printf("[SERVER I/O]    %.6f sec (fwrite 누적)\n", server_times[1]);
    
    /* Cleanup */
    shmdt(shared);
    shmdt(server_times);
    shmdt(counters);
    shmctl(shmid, IPC_RMID, NULL);
    shmctl(server_time_shmid, IPC_RMID, NULL);
    shmctl(counter_shmid, IPC_RMID, NULL);
    semctl(semid, 0, IPC_RMID);
    semctl(sem_ready, 0, IPC_RMID);
    semctl(sem_go_cc, 0, IPC_RMID);
    semctl(sem_done_cc, 0, IPC_RMID);
    semctl(sem_go_cs, 0, IPC_RMID);
    semctl(sem_done_cs, 0, IPC_RMID);
    
    return 0;
}
