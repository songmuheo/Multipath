// main.cpp

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <opencv2/opencv.hpp>

#include "config.h"

namespace fs = std::filesystem;

// 에러 문자열 변환 매크로 제거 (FFmpeg에서 제공하는 av_err2str 함수 사용)
//#define av_err2str(errnum) av_make_error_string((char[AV_ERROR_MAX_STRING_SIZE]){0}, AV_ERROR_MAX_STRING_SIZE, errnum)

// 에러 메시지 헬퍼 함수
static std::string get_av_error(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, sizeof(errbuf));
    return std::string(errbuf);
}

// bin files을 가져오고, sequence 번호대로 정렬한다
std::vector<std::string> get_bin_files_sorted(const std::string& folder_path) {
    std::vector<std::string> bin_files;

    for (const auto& entry : fs::directory_iterator(folder_path)) {
        if (entry.path().extension() == ".bin") {
            bin_files.push_back(entry.path().string());
        }
    }

    // 시퀀스 번호로 정렬
    std::sort(bin_files.begin(), bin_files.end(), [](const std::string& a, const std::string& b) {
        std::string a_filename = fs::path(a).filename().string();
        std::string b_filename = fs::path(b).filename().string();

        int a_seq = std::stoi(a_filename.substr(0, a_filename.find('_')));
        int b_seq = std::stoi(b_filename.substr(0, b_filename.find('_')));

        return a_seq < b_seq;
    });

    return bin_files;
}

// Get frame type
std::string get_frame_type_string(AVFrame* frame) {
    switch (frame->pict_type) {
        case AV_PICTURE_TYPE_I: return "I";
        case AV_PICTURE_TYPE_P: return "P";
        case AV_PICTURE_TYPE_B: return "B";
        case AV_PICTURE_TYPE_S: return "S";
        case AV_PICTURE_TYPE_SI: return "SI";
        case AV_PICTURE_TYPE_SP: return "SP";
        case AV_PICTURE_TYPE_BI: return "BI";
        default: return "Unknown";
    }
}


void process_decoded_frame(AVFrame* frame, int sequence_number, uint64_t timestamp_sending, uint64_t received_time, uint64_t timestamp_frame, const std::string& output_dir) {
    // AVFrame을 OpenCV Mat로 변환
    int width = frame->width;
    int height = frame->height;

    // 스케일러 초기화 (정적 변수로 유지)
    static SwsContext* sws_ctx = nullptr;
    if (!sws_ctx) {
        sws_ctx = sws_getContext(
            width, height, (AVPixelFormat)frame->format,
            width, height, AV_PIX_FMT_BGR24,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
    }

    // 변환된 데이터 저장을 위한 버퍼 생성
    AVFrame* bgr_frame = av_frame_alloc();
    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_BGR24, width, height, 1);
    std::vector<uint8_t> bgr_buffer(num_bytes);
    av_image_fill_arrays(bgr_frame->data, bgr_frame->linesize, bgr_buffer.data(), AV_PIX_FMT_BGR24, width, height, 1);

    // 색상 공간 변환
    sws_scale(sws_ctx, frame->data, frame->linesize, 0, height, bgr_frame->data, bgr_frame->linesize);

    // OpenCV Mat 생성
    cv::Mat img(height, width, CV_8UC3, bgr_buffer.data(), bgr_frame->linesize[0]);

    // 이미지 저장 (파일명에 sequence_number, timestamp, received_time, play_time 포함)
    std::ostringstream oss;
    oss << output_dir << "/"
        << sequence_number << "_"
        << timestamp_sending << "_"
        << received_time << "_"
        << timestamp_frame << ".png";
    std::string frame_filepath = oss.str();

    cv::imwrite(frame_filepath, img);
    av_frame_free(&bgr_frame);
}


void process_stream(const std::string& stream_name) {
    
    // 입력 폴더 및 출력 폴더 설정
    std::string bin_folder = BINS_FILEPATH + stream_name;
    std::string output_dir_base = FRAMES_OUT_FILEPATH + stream_name;

    std::vector<std::string> bin_files = get_bin_files_sorted(bin_folder);

    std::string csv_file_path = CSV_FILEPATH + stream_name + "_log.csv";

    std::ifstream csv_file(csv_file_path);
    std::string header_line;
    std::getline(csv_file, header_line);

    std::istringstream header_stream(header_line);
    std::vector<std::string> headers;
    std::string header;
    while (std::getline(header_stream, header, ',')) {
        headers.push_back(header);
    }

    auto it = std::find(headers.begin(), headers.end(), "sequence number");
    if (it == headers.end()) {
        std::cerr << "sequence number column not found in CSV file." << std::endl;
        return;
    }
    size_t sequence_col_index = std::distance(headers.begin(), it);

    std::map<int, std::vector<std::string>> csv_data;
    std::string line;
    while (std::getline(csv_file, line)) {
        std::istringstream line_stream(line);
        std::vector<std::string> columns;
        std::string column;
        while (std::getline(line_stream, column, ',')) {
            columns.push_back(column);
        }
        int sequence_number = std::stoi(columns[sequence_col_index]);
        csv_data[sequence_number] = columns;
    }
    csv_file.close();
    
    for (const auto& delay: DELAYS){
        std::string delay_label = DELAY_LABELS[&delay - DELAYS];

        // 디코더 초기화
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        AVCodecContext* codec_ctx_measure = avcodec_alloc_context3(codec);  // 디코딩 시간 측정용 디코더
        AVCodecContext* codec_ctx_playback = avcodec_alloc_context3(codec); // 실제 재생용 디코더

        // 디코더 설정 수정
        // 버퍼링을 없애기 위한 ? -> 디코더 안의 버퍼링
        codec_ctx_measure->thread_type = FF_THREAD_SLICE;
        codec_ctx_measure->thread_count = 1;
        codec_ctx_measure->flags |= AV_CODEC_FLAG_LOW_DELAY;

        codec_ctx_playback->thread_type = FF_THREAD_SLICE;
        codec_ctx_playback->thread_count = 1;
        codec_ctx_playback->flags |= AV_CODEC_FLAG_LOW_DELAY;

        if (!codec || !codec_ctx_measure || !codec_ctx_playback) {
            std::cerr << "Codec not found or could not allocate context" << std::endl;
            return;
        }

        // 디코더 열기
        if (avcodec_open2(codec_ctx_measure, codec, nullptr) < 0 || avcodec_open2(codec_ctx_playback, codec, nullptr) < 0) {
            std::cerr << "Could not open codec" << std::endl;
            return;
        }

        // csv file 헤더 추가
        size_t is_use_index = std::find(headers.begin(), headers.end(), "is_use_" + delay_label) - headers.begin();
        size_t encoding_latency_index = std::find(headers.begin(), headers.end(), "encoding_latency") - headers.begin();
        size_t decoding_latency_index = std::find(headers.begin(), headers.end(), "decoding_latency") - headers.begin();
        size_t frame_type_index = std::find(headers.begin(), headers.end(), "frame_type") - headers.begin();

        bool is_use_exists = (is_use_index < headers.size());
        bool encoding_latency_exists = (encoding_latency_index < headers.size());
        bool decoding_latency_exists = (decoding_latency_index < headers.size());
        bool frame_type_exists = (frame_type_index < headers.size());

        if (!is_use_exists) {
            headers.push_back("is_use_" + delay_label);
            is_use_index = headers.size() - 1;
        }
        if (!encoding_latency_exists) {
            headers.push_back("encoding_latency");
            encoding_latency_index = headers.size() - 1;
        }
        if (!decoding_latency_exists) {
            headers.push_back("decoding_latency");
            decoding_latency_index = headers.size() - 1;
        }
        if (!frame_type_exists) {
            headers.push_back("frame_type");
            frame_type_index = headers.size() - 1;
        }
        for (const auto& bin_file : bin_files) {
            // 파일명에서 정보 추출
            std::string filename = fs::path(bin_file).filename().string();
            std::istringstream iss(filename);
            std::string token;
            std::vector<std::string> tokens;
            while (std::getline(iss, token, '_')) {
                tokens.push_back(token);
            }
            if (tokens.size() < 4) {
                std::cerr << "Invalid filename format: " << filename << std::endl;
                continue;
            }
            int sequence_number = std::stoi(tokens[0]);
            uint64_t timestamp_sending = std::stoull(tokens[1]);
            uint64_t received_time = std::stoull(tokens[2]);
            uint64_t timestamp_frame = std::stoull(tokens[3].substr(0, tokens[3].find('.')));

            // 네트워크 지연 계산 (microseconds 단위)
            int64_t network_latency = (received_time - timestamp_sending) / 1000.0;
            double encoding_latency = (timestamp_sending - timestamp_frame) / 1000.0;
            int64_t total_latency = network_latency;
            bool is_use = (total_latency <= delay / 1000);

            // 프레임 데이터 읽기
            std::ifstream infile(bin_file, std::ios::binary);
            if (!infile) {
                std::cerr << "Could not open file: " << bin_file << std::endl;
                continue;
            }

            std::vector<uint8_t> frame_data((std::istreambuf_iterator<char>(infile)),
                                            std::istreambuf_iterator<char>());
            infile.close();

            // 디코딩 시간 측정 시작
            auto decode_start = std::chrono::high_resolution_clock::now();

            // 패킷 생성 및 데이터 설정 (디코딩 시간 측정용)
            AVPacket* packet_measure = av_packet_alloc();
            // av_init_packet(packet_measure); // av_packet_alloc()으로 대체됨
            packet_measure->data = frame_data.data();
            packet_measure->size = frame_data.size();

            // 디코더에 패킷 전송 (디코딩 시간 측정용)
            int ret_measure = avcodec_send_packet(codec_ctx_measure, packet_measure);
            if (ret_measure < 0) {
                std::cerr << "Error sending packet to decoder (measure): " << get_av_error(ret_measure) << std::endl;
                av_packet_free(&packet_measure);
                continue;
            }

            // 디코딩된 프레임 수신 (디코딩 시간 측정용)
            AVFrame* frame_measure = av_frame_alloc();
            ret_measure = avcodec_receive_frame(codec_ctx_measure, frame_measure);

            // 디코딩 시간 측정 종료
            auto decode_end = std::chrono::high_resolution_clock::now();
            double decoding_latency = std::chrono::duration<double, std::micro>(decode_end - decode_start).count();  // milli seconds 단위
            
            // 측정용 디코더에서 사용한 리소스 해제
            av_frame_free(&frame_measure);
            av_packet_free(&packet_measure);

            // csv file 쓰기
            if (csv_data.find(sequence_number) != csv_data.end()) {
                auto& columns = csv_data[sequence_number];
            if (columns.size() <= encoding_latency_index) {
                columns.resize(encoding_latency_index + 1, "");
            }
            columns[encoding_latency_index] = std::to_string(encoding_latency); // 밀리초 단위로 저장

            if (columns.size() <= decoding_latency_index) {
                columns.resize(decoding_latency_index + 1, "");
            }
            columns[decoding_latency_index] = std::to_string(decoding_latency / 1000.0);

            if (columns.size() <= is_use_index) {
                columns.resize(is_use_index + 1, "");
            }
            columns[is_use_index] = is_use ? "true" : "false";
        }


            if (is_use) {
                // 패킷 생성 및 데이터 설정 (재생용 디코더)
                AVPacket* packet_playback = av_packet_alloc();
                // av_init_packet(packet_playback); // av_packet_alloc()으로 대체됨
                packet_playback->data = frame_data.data();
                packet_playback->size = frame_data.size();

                // 디코더에 패킷 전송 (재생용 디코더)
                int ret_playback = avcodec_send_packet(codec_ctx_playback, packet_playback);
                if (ret_playback < 0) {
                    std::cerr << "Error sending packet to decoder (playback): " << get_av_error(ret_playback) << std::endl;
                    av_packet_free(&packet_playback);
                    continue;
                }

                // 디코딩된 프레임 수신 (재생용 디코더)
                AVFrame* frame_playback = av_frame_alloc();
                ret_playback = avcodec_receive_frame(codec_ctx_playback, frame_playback);

                if (ret_playback == 0) {
                    // 디코딩 성공

                    // 프레임 타입 얻기
                    std::string frame_type_string = get_frame_type_string(frame_playback);

                    // CSV 데이터에 프레임 타입 추가
                    if (csv_data.find(sequence_number) != csv_data.end()) {
                        auto& columns = csv_data[sequence_number];
                        if (columns.size() <= frame_type_index) {
                            columns.resize(frame_type_index + 1, "");
                        }
                        columns[frame_type_index] = frame_type_string;
                    }

                    std::string output_dir = output_dir_base + "_delay_" + delay_label;
                    fs::create_directories(output_dir);
                    // 디코딩된 프레임 처리 및 저장
                    process_decoded_frame(frame_playback, sequence_number, timestamp_sending, received_time, timestamp_frame, output_dir);
                    std::cout << "[" << stream_name << " | delay_" << delay_label << "] 프레임 seq_num: " << sequence_number << "이(가) 사용되었습니다." << std::endl;
                    av_frame_free(&frame_playback);
                } else if (ret_playback == AVERROR(EAGAIN) || ret_playback == AVERROR_EOF) {
                    // 추가 프레임이 없음
                    av_frame_free(&frame_playback);
                } else {
                    // 디코딩 실패
                    std::cerr << "[" << stream_name << "] Error receiving frame from decoder (playback): " << get_av_error(ret_playback) << std::endl;
                    av_frame_free(&frame_playback);
                }

                av_packet_free(&packet_playback);
            } else {
                // 조건 불만족으로 프레임 스킵 (재생용 디코더에 패킷 전달하지 않음) - is_use = False
                std::cout << "[" << stream_name << "] 프레임 " << sequence_number << "이(가) 조건 불만족으로 스킵되었습니다." << std::endl;
            }
        }

        // 디코더 해제
        avcodec_free_context(&codec_ctx_measure);
        avcodec_free_context(&codec_ctx_playback);
    }

    std::ofstream csv_out(csv_file_path);
    if (!csv_out.is_open()) {
        std::cerr << "Failed to open CSV file for writing: " << csv_file_path << std::endl;
        return;
    }

    for (size_t i = 0; i < headers.size(); ++i) {
        csv_out << headers[i];
        if (i < headers.size() - 1) {
            csv_out << ",";
        }
    }
    csv_out << "\n";

    for (const auto& [seq_num, data] : csv_data) {
        for (size_t i = 0; i < data.size(); ++i) {
            csv_out << data[i];
            if (i < data.size() - 1) {
                csv_out << ",";
            }
        }
        csv_out << "\n";
    }
    csv_out.close();
}

int main() {
    std::vector<std::string> streams = {"kt", "lg", "combine"};

    for (const auto& stream_name : streams) {
        std::cout << "==============================\n";
        std::cout << "Stream [" << stream_name << "] processing start\n";
        process_stream(stream_name);
        std::cout << "Stream [" << stream_name << "] processing finish\n";
    }

    return 0;
}
