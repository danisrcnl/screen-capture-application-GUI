#include "../include/ScreenRecorder.h"
#include <iostream>
#include <cassert>

using namespace std;

ScreenRecorder::ScreenRecorder() {
    avdevice_register_all();
    status = IDLE;
}

ScreenRecorder::~ScreenRecorder() {
    avformat_free_context(videoInFormatContext);
    avformat_free_context(outFormatContext);
    avcodec_free_context(&videoInCodecContext);
    avcodec_free_context(&videoOutCodecContext);
    sws_freeContext(videoConverter);
    if (audioRec) {
        avformat_free_context(audioInFormatContext);
        avcodec_free_context(&audioInCodecContext);
        avcodec_free_context(&audioOutCodecContext);
        av_audio_fifo_free(audioFifo);
        swr_free(&audioConverter);
    }
}

int ScreenRecorder::stop() {
    if (status != RECORDING && status != PAUSED) {
        this->failReason = "Can't stop with no capture going on.";
        return -1;
    }
    cout << "entered stop" << endl;
    this->executing = false;
    audioInLk.lock();
    videoInLk.lock();
    avformat_close_input(&audioInFormatContext);
    avformat_close_input(&videoInFormatContext);
    audioInLk.unlock();
    videoInLk.unlock();
    videoThread->join();
    if (audioRec)
        audioThread->join();
    outFmtCtxLock.lock();
    int ret = av_write_trailer(outFormatContext);
    outFmtCtxLock.unlock();
    if (ret < 0) throw std::runtime_error("Can not write file trailer.");
    avio_close(outFormatContext->pb);
    std::clog << "Finished recording." << std::endl;
    fflush(stdout);
    status = IDLE;
    return 0;
}

int ScreenRecorder::pause() {
    if (status != RECORDING){
        this->failReason = "Can't pause with no capture going on.";
        return -1;
    }
    this->running = false;
    this->dropframes = 200;
    cout << BOLDMAGENTA << "PAUSED" << RESET << endl;
    status = PAUSED;
    cout << av_audio_fifo_size(audioFifo) << endl;
    av_audio_fifo_reset(audioFifo);
    cout << av_audio_fifo_size(audioFifo) << endl;
    return 0;
}

int ScreenRecorder::resume() {
    if (status != PAUSED){
        this->failReason = "Can't resume if not paused.";
        return -1;
    }
    this->running = true;
    cout << BOLDMAGENTA << "RESUMED" << RESET << endl;
    status = RECORDING;
    return 0;
}

int ScreenRecorder::start(const string& filename, bool audioRec, int oX, int oY, const string& resolution) {
    if (status != IDLE){
        this->failReason = "There's already an ongoing capture.";
        return -1;
    }
    this->outFile = filename;
    this->resolution = resolution.c_str();
    this->oX = std::to_string(oX).c_str();
    this->oY = std::to_string(oY).c_str();
    this->audioRec = audioRec;
    try {
        this->open();
    }
    catch (std::exception& e) {
        this->failReason = e.what();
        return -2;
    }
    try {
        initFile();
    } catch (std::exception& e) {
        this->failReason = e.what();
        return -3;
    }

    running = true;
    executing = true;

    videoThread = new std::thread([this]() {
        try {
            this->decodeEncodeVideo();
        }
        catch (std::exception& e) {
            encodeVErr = current_exception();
            this->failReason = e.what();
        }
    });
    if (audioRec) {
        audioThread = new std::thread([this]() {
            try {
                this->decodeEncodeAudio();
            }
            catch (std::exception& e) {
                encodeAErr = current_exception();
                this->failReason = e.what();
            }
        });
    }
    status = RECORDING;
    return 0;
}

string ScreenRecorder::getError() {
    return failReason;
}

bool ScreenRecorder::checkEncodeError() {
    return (encodeAErr || encodeVErr);
}

void ScreenRecorder::clearEncodeError() {
    encodeAErr = nullptr;
    encodeVErr = nullptr;
}

void ScreenRecorder::open() {
    try {
        openVideo();
    } catch (std::exception& e) {
        throw;
    }

    if (audioRec){
        try {
            openAudio();
        } catch (std::exception& e) {
            throw;
        }
    }
}

void ScreenRecorder::initFile() {
    if((avformat_alloc_output_context2(&outFormatContext, nullptr, nullptr, outFile.c_str())) < 0)
        throw std::runtime_error("Failed to alloc ouput context");

    if((avio_open(&outFormatContext->pb, outFile.c_str(), AVIO_FLAG_WRITE)) < 0)
        throw std::runtime_error("Fail to open output file.");

    initVideoStream();

    if (audioRec)
        initAudioStream();

    if ((avformat_write_header(outFormatContext , &videoOptions)) < 0)
        throw std::runtime_error("Error writing header context.");
}
