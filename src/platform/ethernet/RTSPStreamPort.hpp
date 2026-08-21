// Copyright (c) Orbbec Inc. All Rights Reserved.
// Licensed under the MIT License.

#pragma once
#include "ISourcePort.hpp"
#include "ethernet/rtsp/ObRTSPClient.hpp"
#include "ethernet/rtsp/ObRTPSink.hpp"
#include "ethernet/rtsp/ObUsageEnvironment.hpp"
#include "ethernet/VendorNetDataPort.hpp"

#include <string>
#include <memory>
#include <thread>

namespace libobsensor {

class RTSPStreamPort : public IVideoStreamPort {
public:
    RTSPStreamPort(std::shared_ptr<const RTSPStreamPortInfo> portInfo);
    virtual ~RTSPStreamPort() noexcept override;

    virtual StreamProfileList                     getStreamProfileList() override;
    virtual void                                  startStream(std::shared_ptr<const StreamProfile> profile, MutableFrameCallback callback) override;
    virtual void                                  stopStream(std::shared_ptr<const StreamProfile> profile) override;
    virtual void                                  stopAllStream() override;
    virtual std::shared_ptr<const SourcePortInfo> getSourcePortInfo() const override;

private:
    void stopStream();
    void createClient(std::shared_ptr<const StreamProfile> profile, MutableFrameCallback callback);
    void closeClient();

    /// Sets destroy_ from the event loop thread so doEventLoop() leaves at its next iteration.
    static void handleStopEventLoop(void *clientData);

private:
    std::shared_ptr<const RTSPStreamPortInfo> portInfo_;
    TaskScheduler                            *taskScheduler_;
    UsageEnvironment                         *live555Env_;
    std::thread                               eventLoopThread_;
    EventTriggerId                            stopEventLoopTrigger_;

    /// Only ever touched by the event loop thread while that thread runs, and by the thread that
    /// starts or joins it otherwise.
    char destroy_;
    bool streamStarted_;

    ObRTSPClient                        *currentRtspClient_;
    std::shared_ptr<const StreamProfile> currentStreamProfile_;
    StreamProfileList                    streamProfileList_;
};
}  // namespace libobsensor

