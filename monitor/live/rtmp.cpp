#include "live/rtmp.h"
#include "common/res_code.h"
#include "live/rtmp_streamer.h"

#include <base/ref_counted_object.h>

namespace nvr
{

rtc::scoped_refptr<LiveModule> RtmpLiveImpl::Create(const Params &params)
{
    err_code code;

    rtc::scoped_refptr<RtmpLiveImpl> implemention = new rtc::RefCountedObject<RtmpLiveImpl>();

    code = static_cast<err_code>(implemention->Initialize(params));

    if (KSuccess != code)
    {
        log_e("error:%s", make_error_code(code).message().c_str());
        return nullptr;
    }

    return implemention;
}

int32_t RtmpLiveImpl::Initialize(const Params &params)
{
    if (init_)
        return static_cast<int>(KDupInitialize);

    run_ = true;
    thread_ = std::unique_ptr<std::thread>(new std::thread([this, params]() {
        err_code code;
        RTMPStreamer rtmp_streamer;
        VideoFrame frame;
        bool wait_sps;

        bool init = false;

        uint8_t *temp_buf = (uint8_t *)malloc(BUFFER_LEN);
        if (!temp_buf)
        {
            log_e("malloc buffer failed");
            return;
        }

        while (run_)
        {
            if (!init)
            {
                wait_sps = true;
                code = static_cast<err_code>(rtmp_streamer.Initialize(params.url));
                if (KSuccess != code)
                {
                    log_e("error:%s", make_error_code(code).message().c_str());
                    return;
                }
                init = true;
            }
            {
                std::unique_lock<std::mutex> lock(mux_);
                if (buffer_.Get((uint8_t *)&frame, sizeof(frame)))
                {
                    memcpy(temp_buf, buffer_.GetCurrentPos(), frame.len);
                    frame.data = temp_buf;
                    if (!buffer_.Consume(frame.len))
                    {
                        log_e("consme data from buffer failed,rest data not enough");
                        return;
                    }
                }
                else if (run_)
                {
                    cond_.wait(lock);
                    continue;
                }
            }

            if (frame.type == H264Frame::NaluType::SPS)
                wait_sps = false;

            if (init && !wait_sps)
            {
                code = static_cast<err_code>(rtmp_streamer.WriteVideoFrame(frame));
                if (KSuccess != code)
                {
                    log_w("rtmp connection break,try to reconnect...");
                    rtmp_streamer.Close();
                    init = false;
                }
            }
        }

        rtmp_streamer.Close();
        free(temp_buf);
    }));

    init_ = true;
    return static_cast<int>(KSuccess);
}

void RtmpLiveImpl::OnFrame(const VideoFrame &frame)
{
    if (!init_)
        return;

    mux_.lock();

    if (buffer_.FreeSpace() < sizeof(frame) + frame.len)
    {
        mux_.unlock();
        return;
    }
    buffer_.Append((uint8_t *)&frame, sizeof(frame));
    buffer_.Append(frame.data, frame.len);
    cond_.notify_one();
    mux_.unlock();
}

void RtmpLiveImpl::Close()
{
    if (!init_)
        return;

    run_ = false;
    cond_.notify_all();
    thread_->join();
    thread_.reset();
    thread_ = nullptr;
    buffer_.Clear();
    init_ = false;
}

RtmpLiveImpl::RtmpLiveImpl() : run_(false),
                               thread_(nullptr),
                               init_(false)
{
}

RtmpLiveImpl::~RtmpLiveImpl()
{
    Close();
}
}; // namespace nvr