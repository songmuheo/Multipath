// main.cpp
#include <iostream>
#include <fstream>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <thread>
#include <filesystem>
#include "config.h"

struct PacketHeader {
    uint64_t timestamp;        // 8 bytes, microsecond 단위
    uint32_t sequence_number;   // 4 bytes
};

// 패킷 데이터를 파일에 저장하는 함수
void save_packet_data(const char* filepath, const char* data, size_t size) {
    std::ofstream outfile(filepath, std::ios::binary | std::ios::app);
    outfile.write(data, size);
    outfile.close();
}

// 패킷 정보를 CSV 파일에 기록하는 함수
void log_packet_info(const char* logpath, const std::string& source_ip, uint32_t sequence_number, uint64_t latency_us, size_t message_size) {
    std::ofstream logfile(logpath, std::ios::app);
    logfile << source_ip << "," << sequence_number << "," << (latency_us / 1000.0) << "," << message_size << "\n"; // latency를 ms로 변환하여 기록
    logfile.close();
}

// 현재 날짜와 시간을 기반으로 폴더 경로 생성
std::string create_timestamped_directory(const std::string& base_dir) {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm* local_time = std::localtime(&now_time);

    std::ostringstream folder_name;
    folder_name << std::put_time(local_time, "%Y-%m-%d_%H-%M");

    std::string full_path = base_dir + "/" + folder_name.str();
    std::filesystem::create_directories(full_path + "/logs");
    std::filesystem::create_directories(full_path + "/bins");

    return full_path;
}

// 소켓 설정 및 데이터 수신
void receive_packets(int port, const char* log_filepath, const char* frame_filepath) {
    int sockfd;
    char buffer[BUFFER_SIZE];
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // 소켓 생성
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    memset(&client_addr, 0, sizeof(client_addr));

    // 서버 주소 설정
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    // 소켓에 주소 바인딩
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    std::cout << "Listening on port " << port << std::endl;

    while (true) {
        ssize_t len = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&client_addr, &addr_len);
        if (len < 0) {
            perror("recvfrom failed");
            continue;
        }

        // 패킷 수신 시간 기록 (마이크로초 단위)
        uint64_t received_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::system_clock::now().time_since_epoch()).count();

        // 클라이언트 IP 주소 가져오기
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

        // 사용자 정의 헤더 파싱
        PacketHeader* header = reinterpret_cast<PacketHeader*>(buffer);

        // 패킷 도착 지연 시간 계산 (마이크로초 단위)
        uint64_t latency_us = received_time_us - header->timestamp;

        // 패킷 정보 로그 파일에 기록
        log_packet_info(log_filepath, client_ip, header->sequence_number, latency_us, len);

        // 패킷 데이터 파일로 저장
        std::string frame_filepath_with_seq = std::string(frame_filepath) + "/bins/" + std::to_string(received_time_us) + ".bin";
        save_packet_data(frame_filepath_with_seq.c_str(), buffer + sizeof(PacketHeader), len - sizeof(PacketHeader));

        // std::cout << "Packet received from: " << client_ip << ", Seq: " << header->sequence_number 
        //           << ", Latency: " << (latency_us / 1000.0) << " ms, Size: " << len << " bytes\n";
    }

    close(sockfd);
}

int main() {
    // 새로운 디렉터리 생성
    std::string base_dir = FILEPATH_RAW;
    std::string folder_path = create_timestamped_directory(base_dir);

    std::string port1_log = folder_path + "/logs/lg_log.csv";
    std::string port2_log = folder_path + "/logs/kt_log.csv";

    // 포트 1과 포트 2에서 패킷 수신을 처리하는 쓰레드 생성 (멀티스레드로 처리할 경우)
    std::thread port1_thread(receive_packets, SERVER_PORT1, port1_log.c_str(), folder_path.c_str());
    std::thread port2_thread(receive_packets, SERVER_PORT2, port2_log.c_str(), folder_path.c_str());

    // 쓰레드가 종료될 때까지 대기
    port1_thread.join();
    port2_thread.join();

    return 0;
}