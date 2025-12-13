#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/time.h>
#include <unistd.h>

#define MSG_KEY 0x1234
#define NUM_CLIENT 8
#define NUM_DISK 4
#define CHUNK_INT 256

#define GET_DURATION(start, end) \
    ((end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0)

/* message 구조체 */
struct msgbuf {
    long mtype;                 // client id
    // int  chunk_idx;          // 0 or 1
    int data[CHUNK_INT];        // 1KB
};

int main() {
    int msqid;
    struct msgbuf msg;

    FILE *raid_fp[NUM_DISK];
    char filename[32];

    struct timeval io_start, io_end;
    double io_time = 0.0;
    double c2s_time = 0.0;

    /* msg queue 생성 */
    msqid = msgget(MSG_KEY, IPC_CREAT | 0666);
    if (msqid == -1) {
        perror("msgget failed");
        exit(1);
    }

    /* RAID disk 파일 열기 */
    for (int i = 0; i < NUM_DISK; i++) {
        sprintf(filename, "raid_disk%d.bin", i);
        raid_fp[i] = fopen(filename, "wb");
        if (raid_fp[i] == NULL) {
            perror("fopen failed");
            exit(1);
        }
    }

    printf("=== SERVER (Message Queue 기반 RAID0) 시작 ===\n");
    /* disk별 임시 버퍼 */
    // int temp_buf[4][2][CHUNK_INT];   // [disk][chunk_idx][data]
    // int recv_flag[4][2] = {0};       // 수신 여부

    /* client 수 만큼 수신 */
    struct timeval c2s_start, c2s_end;
    
    for (int c = 0; c < NUM_CLIENT*2; c++) {

        /* -------- client → server -------- */
        gettimeofday(&c2s_start, NULL);
        msgrcv(msqid, &msg, sizeof(msg.data), 0, 0);
        gettimeofday(&c2s_end, NULL);

        c2s_time += GET_DURATION(c2s_start, c2s_end);

        int sm_id  = msg.mtype - 1;
        int disk_id = sm_id % 4;
        // int chunk_id = msg.chunk_idx;   // 0 or 1

        gettimeofday(&io_start, NULL);
        fwrite(msg.data, sizeof(int), CHUNK_INT, raid_fp[disk_id]);

        printf("[SERVER] SM %d → disk %d (256 int 저장)\n",
            sm_id, disk_id);

        fflush(raid_fp[disk_id]);

        gettimeofday(&io_end, NULL);
        io_time += GET_DURATION(io_start, io_end);

        // printf("[SERVER] client %ld → disk %d 저장 완료 (256 int)\n",
        //        msg.mtype, disk_id);
    }

    printf("\n=== client -> server TIME ===\n");
    printf("Total client -> server Time: %.6f sec\n", c2s_time);

    printf("\n=== SERVER I/O TIME ===\n");
    printf("Total Server I/O Time: %.6f sec\n", io_time);

    /* 정리 */
    for (int i = 0; i < NUM_DISK; i++) {
        fclose(raid_fp[i]);
    }

    msgctl(msqid, IPC_RMID, NULL);

    return 0;
}
