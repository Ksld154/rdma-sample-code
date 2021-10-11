#include <fcntl.h>
#include <libgen.h>

#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>

#include "messages.h"
#include "tdma-common.hpp"

struct client_context {
    char *         buffer;
    struct ibv_mr *buffer_mr;

    struct message *msg;
    struct ibv_mr * msg_mr;

    uint64_t peer_addr;
    uint32_t peer_rkey;

    int         fd;
    const char *file_name;
    int         payload_idx;
};

bool                    SENDING_FLAG = true;
bool                    STOP = false;
std::condition_variable cv_flag;
std::mutex              mtx;

static void write_remote(struct rdma_cm_id *id, uint32_t len) {
    struct client_context *ctx = (struct client_context *)id->context;
    struct ibv_send_wr     wr, *bad_wr = NULL;
    struct ibv_sge         sge;

    memset(&wr, 0, sizeof(wr));

    wr.wr_id = (uintptr_t)id;
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.imm_data = htonl(len);  // data(buffer) length
    wr.wr.rdma.remote_addr = ctx->peer_addr;
    wr.wr.rdma.rkey = ctx->peer_rkey;

    if(len) {
        wr.sg_list = &sge;
        wr.num_sge = 1;

        sge.addr = (uintptr_t)ctx->buffer;
        sge.length = len;
        sge.lkey = ctx->buffer_mr->lkey;
    }

    int err = ibv_post_send(id->qp, &wr, &bad_wr);
    if(err != 0) {
        perror("ibv_post_send");
        // std::cout << "error code:" << err << std::endl;
    }
    // TEST_NZ(ibv_post_send(id->qp, &wr, &bad_wr));
}

// for receiving server's control message
static void post_receive(struct rdma_cm_id *id) {
    struct client_context *ctx = (struct client_context *)id->context;
    struct ibv_recv_wr     wr, *bad_wr = NULL;
    struct ibv_sge         sge;

    memset(&wr, 0, sizeof(wr));

    wr.wr_id = (uintptr_t)id;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    // a buffer large enough to hold a struct message
    sge.addr = (uintptr_t)ctx->msg;
    sge.length = sizeof(*ctx->msg);
    sge.lkey = ctx->msg_mr->lkey;

    int err = ibv_post_recv(id->qp, &wr, &bad_wr);
    if(err != 0) {
        perror("ibv_post_recv");
        // std::cout
        // std::cout << "error code:" << err << std::endl;
    }
    // TEST_NZ(ibv_post_recv(id->qp, &wr, &bad_wr));
}

static void send_next_chunk(struct rdma_cm_id *id) {
    struct client_context *ctx = (struct client_context *)id->context;

    ssize_t size = read(ctx->fd, ctx->buffer, BUFFER_SIZE);
    // std::cout << size << std::endl;
    if(size == -1)
        rc_die("read() failed\n");

    ctx->payload_idx++;
    write_remote(id, size);
}

// send filename to server through RDMA_WRITE
static void send_file_name(struct rdma_cm_id *id) {
    struct client_context *ctx = (struct client_context *)id->context;

    // strcpy(ctx->buffer, ctx->file_name);
    strncpy(ctx->buffer, ctx->file_name, strlen(ctx->file_name) + 1);

    write_remote(id, strlen(ctx->file_name) + 1);  //+1 for trailing '\0'
}

static void on_pre_conn(struct rdma_cm_id *id) {
    struct client_context *ctx = (struct client_context *)id->context;

    // for buffering the data chucks that are sending to server
    posix_memalign((void **)&ctx->buffer, sysconf(_SC_PAGESIZE), BUFFER_SIZE);
    TEST_Z(ctx->buffer_mr = ibv_reg_mr(rc_get_pd(), ctx->buffer, BUFFER_SIZE, 0));

    // for receiving server's control message
    posix_memalign((void **)&ctx->msg, sysconf(_SC_PAGESIZE), sizeof(*ctx->msg));
    TEST_Z(ctx->msg_mr = ibv_reg_mr(rc_get_pd(), ctx->msg, sizeof(*ctx->msg), IBV_ACCESS_LOCAL_WRITE));

    ctx->payload_idx = 0;
    post_receive(id);
}

static void on_completion(struct ibv_wc *wc) {
    struct rdma_cm_id *    id = (struct rdma_cm_id *)(uintptr_t)(wc->wr_id);
    struct client_context *ctx = (struct client_context *)id->context;
    uint16_t               src_port = rdma_get_src_port(id);

    if(wc->opcode & IBV_WC_RECV) {
        if(ctx->msg->id == MSG_MR) {
            ctx->peer_addr = ctx->msg->data.mr.addr;
            ctx->peer_rkey = ctx->msg->data.mr.rkey;

            printf("received MR, sending file name\n");
            send_file_name(id);

        } else if(ctx->msg->id == MSG_READY) {
            printf("[%d, %d] received READY, sending chunk\n", src_port, ctx->payload_idx);

            // block until flag flip
            std::unique_lock<std::mutex> lock(mtx);
            cv_flag.wait(lock, [] { return SENDING_FLAG; });  // block until SENDING_FLAG==1
            lock.unlock();

            send_next_chunk(id);
        } else if(ctx->msg->id == MSG_DONE) {
            printf("received DONE, disconnecting\n");
            STOP = true;
            rc_disconnect(id);
            return;
        }

        post_receive(id);
    }
}

void flip_sending_flag() {
    while(1) {
        std::this_thread::sleep_for(std::chrono::seconds(2));

        std::lock_guard<std::mutex> lock(mtx);
        SENDING_FLAG = !SENDING_FLAG;
        if(SENDING_FLAG == true) {
            std::cout << "Flip sending bit " << SENDING_FLAG << std::endl;
            cv_flag.notify_one();
        }
        lock.~lock_guard();

        if(STOP) {
            return;
        }
    }
    return;
}

int main(int argc, char **argv) {
    if(argc != 3) {
        fprintf(stderr, "usage: %s <server-address> <file-name>\n", argv[0]);
        return 1;
    }

    struct client_context ctx;
    ctx.file_name = basename(argv[2]);

    ctx.fd = open(argv[2], O_RDONLY);
    if(ctx.fd == -1) {
        fprintf(stderr, "unable to open input file \"%s\"\n", ctx.file_name);
        return 1;
    }

    auto future = std::async(flip_sending_flag);

    rc_init(
        on_pre_conn,
        NULL,  // on connect
        on_completion,
        NULL);  // on disconnect

    rc_client_loop(argv[1], DEFAULT_PORT, &ctx);

    close(ctx.fd);

    return 0;
}
