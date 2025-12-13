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
#define TOTAL_DATA     4096
#define MATRIX_SIZE    64
#define DOMAIN_SIZE    (TOTAL_DATA / N_CLIENT)
#define LOCAL_CHUNK    512
#define SHM_SIZE       (TOTAL_DATA * sizeof(int))

#define C2S_BLOCK_INTS 512
#define C2S_SHM_SIZE   (N_CLIENT * C2S_BLOCK_INTS * sizeof(int))

#define SEM_INIT_READY_NAME   "/sem_init_ready_sm"
#define SEM_DOMAIN_READY_NAME "/sem_domain_ready_sm"
#define SEM_C2S_READY_NAME    "/sem_c2s_ready_sm"

static void fill_initial_data(int cid, int* buf32) {
    int idx;
    int TILE_SIZE;
    int tile_col;
    int tile_row0, tile_row1;
    int tr;
    int tile_row;
    int r, c, v;

    idx = 0;
    TILE_SIZE = 16;

    tile_col = cid % 4;
    tile_row0 = (cid / 4) * 2;
    tile_row1 = tile_row0 + 1;

    for (tr = 0; tr < 2; tr++) {
        tile_row = (tr == 0) ? tile_row0 : tile_row1;
        for (r = tile_row * TILE_SIZE; r < (tile_row + 1) * TILE_SIZE; r++) {
            for (c = tile_col * TILE_SIZE; c < (tile_col + 1) * TILE_SIZE; c++) {
                v = r * MATRIX_SIZE + c;
                buf32[idx++] = v;
            }
        }
    }
}

static void client_process_shm(int cid, int initial_shmid, int domain_shmid, int c2s_shmid) {
    int initial[LOCAL_CHUNK];
    int domain[DOMAIN_SIZE];
    int* shm_initial;
    int* shm_domain;
    int* shm_c2s;
    char fname[64];
    FILE* fp;

    sem_t* init_ready_sem;
    sem_t* domain_ready_sem;
    sem_t* c2s_ready_sem;

    init_ready_sem = sem_open(SEM_INIT_READY_NAME, 0);
    domain_ready_sem = sem_open(SEM_DOMAIN_READY_NAME, 0);
    c2s_ready_sem = sem_open(SEM_C2S_READY_NAME, 0);
    if (init_ready_sem == SEM_FAILED || domain_ready_sem == SEM_FAILED || c2s_ready_sem == SEM_FAILED) {
        perror("sem_open in client");
        exit(1);
    }

    shm_initial = (int*)shmat(initial_shmid, NULL, 0);
    shm_domain  = (int*)shmat(domain_shmid, NULL, 0);
    shm_c2s     = (int*)shmat(c2s_shmid, NULL, 0);
    if (shm_initial == (void*)-1 || shm_domain == (void*)-1 || shm_c2s == (void*)-1) {
        perror("shmat in client");
        exit(1);
    }

    fill_initial_data(cid, initial);
    memcpy(&shm_initial[cid * LOCAL_CHUNK], initial, LOCAL_CHUNK * sizeof(int));

    snprintf(fname, sizeof(fname), "client%d_dist.dat", cid);
    fp = fopen(fname, "wb");
    if (!fp) {
        perror("fopen client_dist.dat");
        exit(1);
    }
    fwrite(initial, sizeof(int), LOCAL_CHUNK, fp);
    fclose(fp);

    sem_post(init_ready_sem);

    sem_wait(domain_ready_sem);

    memcpy(domain, &shm_domain[cid * DOMAIN_SIZE], DOMAIN_SIZE * sizeof(int));

    snprintf(fname, sizeof(fname), "client%d_domain.dat", cid);
    fp = fopen(fname, "wb");
    if (!fp) {
        perror("fopen client_domain.dat");
        exit(1);
    }
    fwrite(domain, sizeof(int), DOMAIN_SIZE, fp);
    fclose(fp);

    memcpy(&shm_c2s[cid * C2S_BLOCK_INTS], domain, C2S_BLOCK_INTS * sizeof(int));
    sem_post(c2s_ready_sem);

    shmdt(shm_initial);
    shmdt(shm_domain);
    shmdt(shm_c2s);

    sem_close(init_ready_sem);
    sem_close(domain_ready_sem);
    sem_close(c2s_ready_sem);

    exit(0);
}

int main(void) {
    int initial_shmid, domain_shmid, c2s_shmid;
    int* shm_initial;
    int* shm_domain;
    int temp_domain[N_CLIENT][DOMAIN_SIZE];
    int domain_count[N_CLIENT];

    struct timeval start, end;
    double elapsed;

    sem_t* init_ready_sem;
    sem_t* domain_ready_sem;
    sem_t* c2s_ready_sem;

    int cid;
    int i;
    pid_t pid;

    init_ready_sem = sem_open(SEM_INIT_READY_NAME, O_CREAT, 0666, 0);
    domain_ready_sem = sem_open(SEM_DOMAIN_READY_NAME, O_CREAT, 0666, 0);
    c2s_ready_sem = sem_open(SEM_C2S_READY_NAME, O_CREAT, 0666, 0);
    if (init_ready_sem == SEM_FAILED || domain_ready_sem == SEM_FAILED || c2s_ready_sem == SEM_FAILED) {
        perror("sem_open in parent");
        return 1;
    }

    initial_shmid = shmget(IPC_PRIVATE, SHM_SIZE, IPC_CREAT | 0666);
    domain_shmid  = shmget(IPC_PRIVATE, SHM_SIZE, IPC_CREAT | 0666);
    c2s_shmid     = shmget(IPC_PRIVATE, C2S_SHM_SIZE, IPC_CREAT | 0666);
    if (initial_shmid < 0 || domain_shmid < 0 || c2s_shmid < 0) {
        perror("shmget");
        return 1;
    }

    shm_initial = (int*)shmat(initial_shmid, NULL, 0);
    shm_domain  = (int*)shmat(domain_shmid, NULL, 0);
    if (shm_initial == (void*)-1 || shm_domain == (void*)-1) {
        perror("shmat in parent");
        return 1;
    }

    for (cid = 0; cid < N_CLIENT; cid++) {
        pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            client_process_shm(cid, initial_shmid, domain_shmid, c2s_shmid);
        }
    }

    printf("Parent waiting for %d clients to write initial data to shared memory...\n", N_CLIENT);
    for (i = 0; i < N_CLIENT; i++) sem_wait(init_ready_sem);
    printf("All clients ready. Starting data redistribution...\n");

    gettimeofday(&start, NULL);

    for (i = 0; i < N_CLIENT; i++) domain_count[i] = 0;

    for (i = 0; i < TOTAL_DATA; i++) {
        int v;
        int owner;
        int pos;

        v = shm_initial[i];
        owner = v / DOMAIN_SIZE;
        pos   = v % DOMAIN_SIZE;

        if (owner < 0 || owner >= N_CLIENT || pos < 0 || pos >= DOMAIN_SIZE) {
            fprintf(stderr, "bad v=%d owner=%d pos=%d\n", v, owner, pos);
            exit(1);
        }

        temp_domain[owner][pos] = v;
        domain_count[owner]++;
    }

    for (i = 0; i < N_CLIENT; i++) {
        if (domain_count[i] != DOMAIN_SIZE) {
            fprintf(stderr, "domain_count mismatch: client %d got %d ints (expected %d)\n",
                    i, domain_count[i], DOMAIN_SIZE);
            exit(1);
        }
    }

    for (i = 0; i < N_CLIENT; i++) {
        memcpy(&shm_domain[i * DOMAIN_SIZE], temp_domain[i], DOMAIN_SIZE * sizeof(int));
    }

    gettimeofday(&end, NULL);
    elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Client-Client communication (via Shared Memory Redistribution) elapsed time: %.6f sec\n", elapsed);

    for (i = 0; i < N_CLIENT; i++) sem_post(domain_ready_sem);
    for (i = 0; i < N_CLIENT; i++) wait(NULL);

    shmdt(shm_initial);
    shmdt(shm_domain);
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
