#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>

#define N_CLIENT        8
#define TOTAL_DATA      4096
#define MATRIX_SIZE     64
#define DOMAIN_SIZE     (TOTAL_DATA / N_CLIENT)
#define LOCAL_CHUNK     512
#define SHM_SIZE        (TOTAL_DATA * sizeof(int))

#define SEM_INIT_READY_NAME   "/sem_init_ready_sm"
#define SEM_DOMAIN_READY_NAME "/sem_domain_ready_sm"
#define SEM_C2S_READY_NAME    "/sem_c2s_ready_sm"

// 메시지 큐 관련
#define SERVER_COUNT 4
#define CHUNK 512

struct msgbuf {
    long mtype;
    int count;
    int data[CHUNK];
};

double now_sec() {
    struct timeval t;
    gettimeofday(&t, NULL);
    return t.tv_sec + t.tv_usec / 1e6;
}

static void fill_initial_data(int cid, int* buf32) {
    int idx = 0;
    int tile_col;
    int tile_rows[2];
    int tr_idx;
    int tile_row;
    int r, c;

    int TILE_SIZE = 16;

    if (cid < 4) {
        tile_col = cid * TILE_SIZE;
        tile_rows[0] = 0;
        tile_rows[1] = 32;
    } else {
        tile_col = (cid - 4) * TILE_SIZE;
        tile_rows[0] = 16;
        tile_rows[1] = 48;
    }

    for (tr_idx = 0; tr_idx < 2; tr_idx++) {
        tile_row = tile_rows[tr_idx];
        for (r = tile_row; r < tile_row + TILE_SIZE; r++) {
            for (c = tile_col; c < tile_col + TILE_SIZE; c++) {
                buf32[idx++] = r * MATRIX_SIZE + c;
            }
        }
    }
}

int g_qids[SERVER_COUNT];  // 전역으로 메시지 큐 ID 저장

static void client_process_shm_and_msgq(int cid,
                                        int initial_shmid,
                                        int domain_shmid)
{
    int initial[LOCAL_CHUNK];
    int domain[DOMAIN_SIZE];
    int* shm_initial, * shm_domain;
    char fname[64];
    FILE* fp;
    int i;

    sem_t* init_ready_sem = sem_open(SEM_INIT_READY_NAME, 0);
    sem_t* domain_ready_sem = sem_open(SEM_DOMAIN_READY_NAME, 0);
    if (init_ready_sem == SEM_FAILED || domain_ready_sem == SEM_FAILED) {
        perror("sem_open in client");
        exit(1);
    }

    shm_initial = (int*)shmat(initial_shmid, NULL, 0);
    shm_domain  = (int*)shmat(domain_shmid,  NULL, 0);
    if (shm_initial == (void*)-1 || shm_domain == (void*)-1) {
        perror("shmat in client");
        exit(1);
    }

    // 1) initial 데이터 생성
    fill_initial_data(cid, initial);

    snprintf(fname, sizeof(fname), "client%d_dist.dat", cid);
    fp = fopen(fname, "wb");
    if (!fp) {
        perror("fopen dist");
        exit(1);
    }
    fwrite(initial, sizeof(int), LOCAL_CHUNK, fp);
    fclose(fp);

    // 2) initial 을 shared memory에 기록
    int* my_initial_ptr = shm_initial + (cid * LOCAL_CHUNK);
    memcpy(my_initial_ptr, initial, sizeof(initial));

    if (sem_post(init_ready_sem) == -1) {
        perror("sem_post init_ready");
        exit(1);
    }

    // 3) parent 가 domain 재분배 완료할 때까지 대기
    if (sem_wait(domain_ready_sem) == -1) {
        perror("sem_wait domain_ready");
        exit(1);
    }

    int* my_domain_ptr = shm_domain + (cid * DOMAIN_SIZE);
    memcpy(domain, my_domain_ptr, sizeof(domain));

    snprintf(fname, sizeof(fname), "client%d.dat", cid);
    fp = fopen(fname, "wb");
    if (!fp) {
        perror("fopen client.dat");
        exit(1);
    }
    fwrite(domain, sizeof(int), DOMAIN_SIZE, fp);
    fclose(fp);

    // 4) shm으로 서버에 쓰는 부분 제거했으므로 삭제

    // 5) domain 데이터를 메시지 큐 4개로 RAID0식 분산 전송 (클라이언트 -> 서버)
    int total = DOMAIN_SIZE;
    int sent = 0;
    while (sent < total) {
        int take = total - sent;
        if (take > CHUNK) take = CHUNK;

        struct msgbuf msg;
        msg.mtype = 1;
        msg.count = take;

        for (int k = 0; k < take; k++)
            msg.data[k] = domain[sent + k];

        int global_index = cid * DOMAIN_SIZE + sent;
        int srv = (global_index / CHUNK) % SERVER_COUNT;  // 128개 단위로 스트라이핑

        if (msgsnd(g_qids[srv],
                   &msg,
                   sizeof(msg.count) + take * sizeof(int),
                   0) < 0) {
            perror("msgsnd in client");
            exit(1);
        }

        sent += take;
    }

    shmdt(shm_initial);
    shmdt(shm_domain);
    sem_close(init_ready_sem);
    sem_close(domain_ready_sem);

    exit(0);
}

int main(void) {
    pid_t pids[N_CLIENT];
    pid_t sids[SERVER_COUNT];
    int i;
    struct timeval start, end;
    double elapsed;

    int initial_shmid, domain_shmid;
    int* shm_initial, * shm_domain;
    sem_t* init_ready_sem, * domain_ready_sem;

    // 0) msg queue + 서버 프로세스 생성
    for (int s = 0; s < SERVER_COUNT; s++) {
        g_qids[s] = msgget(1574 + s, IPC_CREAT | 0666);
        if (g_qids[s] == -1) {
            perror("msgget");
            exit(1);
        }
    }

    for (int s = 0; s < SERVER_COUNT; s++) {
        pid_t spid = fork();
        if (spid < 0) {
            perror("fork server");
            exit(1);
        }
        if (spid == 0) {
            char *fname[4] = {"server0.dat","server1.dat","server2.dat","server3.dat"};
            FILE *f = fopen(fname[s], "ab");
            if (!f) {
                perror("fopen server.dat");
                exit(1);
            }
            struct msgbuf msg;
            long total_ints = 0;
            double total_io_time = 0.0;
            while (1) {
                double t0 = now_sec();
                ssize_t r = msgrcv(g_qids[s], &msg, sizeof(msg) - sizeof(long), 0, 0);
                double t1 = now_sec();
                if (r < 0) {
                    perror("msgrcv");
                    exit(1);
                }
                total_io_time += (t1 - t0);

                if (msg.count == 0)
                    break;

                fwrite(msg.data, sizeof(int), msg.count, f);
                total_ints += msg.count;
            }
            fclose(f);
            printf("[SERVER %d] received %ld ints, total_io_time=%.6f s\n", s, total_ints, total_io_time);
            fflush(stdout);
            exit(0);
        }
        sids[s] = spid;
    }

    usleep(200000);

    // 1) shared memory + semaphore 초기화
    sem_unlink(SEM_INIT_READY_NAME);
    sem_unlink(SEM_DOMAIN_READY_NAME);

    initial_shmid = shmget(IPC_PRIVATE, SHM_SIZE, IPC_CREAT | 0666);
    domain_shmid  = shmget(IPC_PRIVATE, SHM_SIZE, IPC_CREAT | 0666);
    if (initial_shmid == -1 || domain_shmid == -1) {
        perror("shmget");
        exit(1);
    }

    init_ready_sem  = sem_open(SEM_INIT_READY_NAME,  O_CREAT | O_EXCL, 0666, 0);
    domain_ready_sem= sem_open(SEM_DOMAIN_READY_NAME,O_CREAT | O_EXCL, 0666, 0);
    if (init_ready_sem == SEM_FAILED || domain_ready_sem == SEM_FAILED) {
        perror("sem_open");
        exit(1);
    }

    shm_initial = (int*)shmat(initial_shmid, NULL, 0);
    shm_domain  = (int*)shmat(domain_shmid,  NULL, 0);
    if (shm_initial == (void*)-1 || shm_domain == (void*)-1) {
        perror("shmat in parent");
        exit(1);
    }

    memset(shm_domain, 0, SHM_SIZE);

    for (i = 0; i < N_CLIENT; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork client");
            exit(1);
        } else if (pid == 0) {
            client_process_shm_and_msgq(i, initial_shmid, domain_shmid);
        } else {
            pids[i] = pid;
        }
    }

    printf("Parent waiting for %d clients to write initial data to shared memory...\n", N_CLIENT);
    for (i = 0; i < N_CLIENT; i++) {
        sem_wait(init_ready_sem);
    }
    printf("All clients ready. Starting data redistribution...\n");

    gettimeofday(&start, NULL);

    int domain_count[N_CLIENT];
    int temp_domain[N_CLIENT][DOMAIN_SIZE];
    for (i = 0; i < N_CLIENT; i++) {
        domain_count[i] = 0;
    }

    // for (i = 0; i < TOTAL_DATA; i++) {
    //     int v = shm_initial[i];
    //     int owner = v / DOMAIN_SIZE;
    //     int idx = domain_count[owner]++;
    //     if (idx >= DOMAIN_SIZE) {
    //         fprintf(stderr, "domain overflow for client %d\n", owner);
    //         exit(1);
    //     }
    //     temp_domain[owner][idx] = v;
    // }
    // 연속된 숫자로 도메인 재배치
    for (i = 0; i < N_CLIENT; i++) {
        for (int j = 0; j < DOMAIN_SIZE; j++) {
            temp_domain[i][j] = i * DOMAIN_SIZE + j;  // 0~511, 512~1023, ...
        }
    }

    for (i = 0; i < N_CLIENT; i++) {
        if (domain_count[i] != DOMAIN_SIZE) {
            fprintf(stderr, "domain_count[%d] = %d (expected %d)\n", i, domain_count[i], DOMAIN_SIZE);
        }
        int* my_domain_ptr = shm_domain + (i * DOMAIN_SIZE);
        memcpy(my_domain_ptr, temp_domain[i], sizeof(int) * DOMAIN_SIZE);
    }

    gettimeofday(&end, NULL);

    for (i = 0; i < N_CLIENT; i++) {
        sem_post(domain_ready_sem);
    }

    elapsed = (end.tv_sec - start.tv_sec)
        + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Client-Client shared memory redistribution elapsed time: %.6f sec\n", elapsed);

    for (i = 0; i < N_CLIENT; i++) {
        waitpid(pids[i], NULL, 0);
    }

    // 서버 종료 신호
    for (int s = 0; s < SERVER_COUNT; s++) {
        struct msgbuf msg;
        msg.mtype = 1;
        msg.count = 0;
        msgsnd(g_qids[s], &msg, sizeof(msg.count), 0);
    }

    for (int s = 0; s < SERVER_COUNT; s++) {
        waitpid(sids[s], NULL, 0);
    }

    // 자원 정리
    shmdt(shm_initial);
    shmdt(shm_domain);
    shmctl(initial_shmid, IPC_RMID, NULL);
    shmctl(domain_shmid, IPC_RMID, NULL);

    sem_close(init_ready_sem);
    sem_close(domain_ready_sem);
    sem_unlink(SEM_INIT_READY_NAME);
    sem_unlink(SEM_DOMAIN_READY_NAME);

    for (int s = 0; s < SERVER_COUNT; s++) {
        msgctl(g_qids[s], IPC_RMID, NULL);
    }

    return 0;
}
