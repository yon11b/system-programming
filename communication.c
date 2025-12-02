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

#define N_CLIENT       8
#define TOTAL_DATA     256
#define MATRIX_SIZE    16
#define DOMAIN_SIZE    (TOTAL_DATA / N_CLIENT)
#define LOCAL_CHUNK    32
#define SHM_SIZE       (TOTAL_DATA * sizeof(int))

#define C2S_BLOCK_INTS 256
#define C2S_SHM_SIZE   (N_CLIENT * C2S_BLOCK_INTS * sizeof(int))

#define SEM_INIT_READY_NAME   "/sem_init_ready_sm"
#define SEM_DOMAIN_READY_NAME "/sem_domain_ready_sm"
#define SEM_C2S_READY_NAME    "/sem_c2s_ready_sm"

static void fill_initial_data(int cid, int* buf32) {
    int idx = 0;
    int tile_col;
    int tile_rows[2];
    int tr_idx;
    int tile_row;
    int r, c;
    int v;

    if (cid < 4) {
        tile_col = cid;
        tile_rows[0] = 0;
        tile_rows[1] = 2;
    }
    else {
        tile_col = cid - 4;
        tile_rows[0] = 1;
        tile_rows[1] = 3;
    }

    for (tr_idx = 0; tr_idx < 2; tr_idx++) {
        tile_row = tile_rows[tr_idx];
        for (r = tile_row * 4; r < tile_row * 4 + 4; r++) {
            for (c = tile_col * 4; c < tile_col * 4 + 4; c++) {
                v = r * MATRIX_SIZE + c;
                buf32[idx++] = v;
            }
        }
    }
}

static void client_process_shm(int cid, int initial_shmid, int domain_shmid, int c2s_shmid) {
    int initial[LOCAL_CHUNK];
    int domain[DOMAIN_SIZE];
    int* shm_initial, * shm_domain, * shm_c2s;
    char fname[32];
    FILE* fp;
    int i;

    sem_t* init_ready_sem = sem_open(SEM_INIT_READY_NAME, 0);
    sem_t* domain_ready_sem = sem_open(SEM_DOMAIN_READY_NAME, 0);
    sem_t* c2s_ready_sem = sem_open(SEM_C2S_READY_NAME, 0);
    if (init_ready_sem == SEM_FAILED || domain_ready_sem == SEM_FAILED || c2s_ready_sem == SEM_FAILED) {
        perror("sem_open in client");
        exit(1);
    }

    shm_initial = (int*)shmat(initial_shmid, NULL, 0);
    shm_domain = (int*)shmat(domain_shmid, NULL, 0);
    shm_c2s = (int*)shmat(c2s_shmid, NULL, 0);
    if (shm_initial == (void*)-1 || shm_domain == (void*)-1 || shm_c2s == (void*)-1) {
        perror("shmat in client");
        exit(1);
    }

    fill_initial_data(cid, initial);

    snprintf(fname, sizeof(fname), "client%d_dist.dat", cid);
    fp = fopen(fname, "w");
    if (!fp) {
        perror("fopen dist");
        exit(1);
    }
    for (i = 0; i < LOCAL_CHUNK; i++) {
        fprintf(fp, "%d\n", initial[i]);
    }
    fclose(fp);

    int* my_initial_ptr = shm_initial + (cid * LOCAL_CHUNK);
    memcpy(my_initial_ptr, initial, sizeof(initial));

    if (sem_post(init_ready_sem) == -1) {
        perror("sem_post init_ready");
        exit(1);
    }

    if (sem_wait(domain_ready_sem) == -1) {
        perror("sem_wait domain_ready");
        exit(1);
    }

    int* my_domain_ptr = shm_domain + (cid * DOMAIN_SIZE);
    memcpy(domain, my_domain_ptr, sizeof(domain));

    snprintf(fname, sizeof(fname), "client%d.dat", cid);
    fp = fopen(fname, "w");
    if (!fp) {
        perror("fopen client.dat");
        exit(1);
    }
    for (i = 0; i < DOMAIN_SIZE; i++) {
        fprintf(fp, "%d\n", domain[i]);
    }
    fclose(fp);

    int sendbuf[C2S_BLOCK_INTS];
    for (i = 0; i < DOMAIN_SIZE; i++) {
        sendbuf[i] = domain[i];
    }
    for (i = DOMAIN_SIZE; i < C2S_BLOCK_INTS; i++) {
        sendbuf[i] = 0;
    }

    int* my_c2s_ptr = shm_c2s + (cid * C2S_BLOCK_INTS);
    memcpy(my_c2s_ptr, sendbuf, sizeof(sendbuf));

    if (sem_post(c2s_ready_sem) == -1) {
        perror("sem_post c2s_ready");
        exit(1);
    }

    shmdt(shm_initial);
    shmdt(shm_domain);
    shmdt(shm_c2s);
    sem_close(init_ready_sem);
    sem_close(domain_ready_sem);
    sem_close(c2s_ready_sem);

    exit(0);
}

int main(void) {
    pid_t pids[N_CLIENT];
    int i;
    struct timeval start, end;
    double elapsed;
    struct timeval start_c2s, end_c2s;
    double elapsed_c2s;

    int initial_shmid, domain_shmid, c2s_shmid;
    int* shm_initial, * shm_domain, * shm_c2s;

    sem_t* init_ready_sem, * domain_ready_sem, * c2s_ready_sem;

    sem_unlink(SEM_INIT_READY_NAME);
    sem_unlink(SEM_DOMAIN_READY_NAME);
    sem_unlink(SEM_C2S_READY_NAME);

    initial_shmid = shmget(IPC_PRIVATE, SHM_SIZE, IPC_CREAT | 0666);
    domain_shmid = shmget(IPC_PRIVATE, SHM_SIZE, IPC_CREAT | 0666);
    c2s_shmid = shmget(IPC_PRIVATE, C2S_SHM_SIZE, IPC_CREAT | 0666);
    if (initial_shmid == -1 || domain_shmid == -1 || c2s_shmid == -1) {
        perror("shmget");
        exit(1);
    }

    init_ready_sem = sem_open(SEM_INIT_READY_NAME, O_CREAT | O_EXCL, 0666, 0);
    domain_ready_sem = sem_open(SEM_DOMAIN_READY_NAME, O_CREAT | O_EXCL, 0666, 0);
    c2s_ready_sem = sem_open(SEM_C2S_READY_NAME, O_CREAT | O_EXCL, 0666, 0);
    if (init_ready_sem == SEM_FAILED || domain_ready_sem == SEM_FAILED || c2s_ready_sem == SEM_FAILED) {
        perror("sem_open");
        exit(1);
    }

    shm_initial = (int*)shmat(initial_shmid, NULL, 0);
    shm_domain = (int*)shmat(domain_shmid, NULL, 0);
    shm_c2s = (int*)shmat(c2s_shmid, NULL, 0);
    if (shm_initial == (void*)-1 || shm_domain == (void*)-1 || shm_c2s == (void*)-1) {
        perror("shmat in parent");
        exit(1);
    }

    memset(shm_domain, 0, SHM_SIZE);
    memset(shm_c2s, 0, C2S_SHM_SIZE);

    for (i = 0; i < N_CLIENT; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(1);
        }
        else if (pid == 0) {
            client_process_shm(i, initial_shmid, domain_shmid, c2s_shmid);
        }
        else {
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

    for (i = 0; i < TOTAL_DATA; i++) {
        int v = shm_initial[i];
        int owner = v / DOMAIN_SIZE;
        int idx = domain_count[owner]++;
        if (idx >= DOMAIN_SIZE) {
            fprintf(stderr, "domain overflow for client %d\n", owner);
            exit(1);
        }
        temp_domain[owner][idx] = v;
    }

    for (i = 0; i < N_CLIENT; i++) {
        if (domain_count[i] != DOMAIN_SIZE) {
            fprintf(stderr, "domain_count[%d] = %d (expected %d)\n",
                i, domain_count[i], DOMAIN_SIZE);
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
    printf("Client-Client communication (via Shared Memory Redistribution) elapsed time: %.6f sec\n",
        elapsed);

    gettimeofday(&start_c2s, NULL);
    for (i = 0; i < N_CLIENT; i++) {
        sem_wait(c2s_ready_sem);
    }
    gettimeofday(&end_c2s, NULL);

    elapsed_c2s = (end_c2s.tv_sec - start_c2s.tv_sec)
        + (end_c2s.tv_usec - start_c2s.tv_usec) / 1000000.0;
    printf("Client-Server communication (1KB per client via Shared Memory) elapsed time: %.6f sec\n",
        elapsed_c2s);

    for (i = 0; i < N_CLIENT; i++) {
        waitpid(pids[i], NULL, 0);
    }

    shmdt(shm_initial);
    shmdt(shm_domain);
    shmdt(shm_c2s);
    shmctl(initial_shmid, IPC_RMID, NULL);
    shmctl(domain_shmid, IPC_RMID, NULL);
    shmctl(c2s_shmid, IPC_RMID, NULL);

    sem_close(init_ready_sem);
    sem_close(domain_ready_sem);
    sem_close(c2s_ready_sem);
    sem_unlink(SEM_INIT_READY_NAME);
    sem_unlink(SEM_DOMAIN_READY_NAME);
    sem_unlink(SEM_C2S_READY_NAME);

    return 0;
}
