#include <iostream>
#include <fstream>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <arpa/inet.h>
#include <opencv2/opencv.hpp>
#include <unistd.h>
#include "config.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

using namespace std;

class VideoReceiver {
public:
    VideoReceiver(int port, const string& output_filename)
        : port(port), output_filename(output_filename), running(true) {
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            perror("Socket creation failed");
            exit(EXIT_FAILURE);
        }

        memset(&servaddr, 0, sizeof(servaddr));

        servaddr.sin_family = AF_INET;
        servaddr.sin_port = htons(port);
        servaddr.sin_addr.s_addr = inet_addr(SERVER_IP);

        if (bind(sockfd, (const struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
            perror("Bind failed");
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        output_file.open(output_filename, ios::out | ios::binary);
        if (!output_file.is_open()) {
            cerr << "Could not open output file: " << output_filename << endl;
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        // FFmpeg 초기화
        avcodec_register_all();
        codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            cerr << "Codec not found" << endl;
            exit(EXIT_FAILURE);
        }

        codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            cerr << "Could not allocate video codec context" << endl;
            exit(EXIT_FAILURE);
        }

        if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
            cerr << "Could not open codec" << endl;
            exit(EXIT_FAILURE);
        }

        frame = av_frame_alloc();
        if (!frame) {
            cerr << "Could not allocate video frame" << endl;
            exit(EXIT_FAILURE);
        }

        pkt = av_packet_alloc();
        if (!pkt) {
            cerr << "Could not allocate AVPacket" << endl;
            exit(EXIT_FAILURE);
        }
    }

    ~VideoReceiver() {
        running = false;
        if (recv_thread.joinable()) {
            recv_thread.join();
        }
        output_file.close();
        close(sockfd);
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&codec_ctx);
    }

    void start() {
        recv_thread = thread(&VideoReceiver::receive_and_save, this);
    }

    void receive_and_save() {
        char buffer[PACKET_SIZE];
        socklen_t len;
        struct sockaddr_in cliaddr;
        struct SwsContext* sws_ctx = nullptr;
        cv::Mat img;

        cv::namedWindow("Video", cv::WINDOW_NORMAL);

        while (running) {
            len = sizeof(cliaddr);
            int n = recvfrom(sockfd, buffer, PACKET_SIZE, 0, (struct sockaddr*)&cliaddr, &len);
            if (n > 0) {
                // 수신된 데이터를 파일에 저장
                output_file.write(buffer, n);

                // 패킷 설정
                pkt->data = reinterpret_cast<uint8_t*>(buffer);
                pkt->size = n;

                // 디코딩
                if (avcodec_send_packet(codec_ctx, pkt) >= 0) {
                    while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                        // 스케일링 설정
                        if (!sws_ctx) {
                            sws_ctx = sws_getContext(codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
                                                     codec_ctx->width, codec_ctx->height, AV_PIX_FMT_BGR24,
                                                     SWS_BILINEAR, nullptr, nullptr, nullptr);

                            img = cv::Mat(codec_ctx->height, codec_ctx->width, CV_8UC3);
                        }

                        // 프레임을 BGR로 변환
                        uint8_t* data[1] = { img.data };
                        int linesize[1] = { static_cast<int>(img.step1()) };
                        sws_scale(sws_ctx, frame->data, frame->linesize, 0, codec_ctx->height, data, linesize);

                        // 비디오 창에 표시
                        cv::imshow("Video", img);
                        if (cv::waitKey(1) == 27) {  // ESC 키를 누르면 종료
                            running = false;
                            break;
                        }
                    }
                }
            }
        }

        if (sws_ctx) {
            sws_freeContext(sws_ctx);
        }
    }

private:
    int sockfd;
    int port;
    string output_filename;
    ofstream output_file;
    atomic<bool> running;
    thread recv_thread;
    struct sockaddr_in servaddr;

    // FFmpeg 관련 변수
    AVCodec* codec;
    AVCodecContext* codec_ctx;
    AVFrame* frame;
    AVPacket* pkt;
};

int main() {
    // 두 개의 비디오 수신기를 각각의 포트에서 실행
    VideoReceiver receiver1(SERVER_PORT, "output_video1.h264");
    VideoReceiver receiver2(SERVER_PORT + 1, "output_video2.h264");

    receiver1.start();
    receiver2.start();

    // 프로그램 종료 대기
    cout << "Press Enter to stop..." << endl;
    cin.get();

    return 0;
}
