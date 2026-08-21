// Copyright (c) Orbbec Inc. All Rights Reserved.
// Licensed under the MIT License.

#pragma once
#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include "ObRTPSink.hpp"
#include <atomic>
#include <string>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace libobsensor {

const static uint16_t RTSP_RESPONSE_TIMEOUT_MS          = 12000;
const static uint16_t RTSP_RESPONSE_TIMEOUT_TEARDOWN_MS = 2000;
const static bool     RTP_STREAMING_OVER_TCP            = true;

enum CommandState {
    CMD_RESP_WITH_ERROR = -2,
    CMD_TIMEOUT         = -1,
    CMD_WAITING_RESP    = 0,
    CMD_DONE            = 1,
};

enum RTSPState {
    RTSP_DESCRIBE = 0,
    RTSP_SETUP,
    RTSP_PLAY,
    RTSP_TEARDOWN,
};

class ObRTSPClient : public RTSPClient {
public:
    static ObRTSPClient *createNew(std::shared_ptr<const StreamProfile> profile, UsageEnvironment &env, char const *rtspURL, MutableFrameCallback callback, int verbosityLevel = 0,
                                   portNumBits tunnelOverHTTPPortNum = 0, int socketNumToServer = -1);

    virtual ~ObRTSPClient() noexcept override;

public:
    void startStream();
    void stopStream();

    /// Records which thread runs the live555 event loop for this client's environment.
    ///
    /// Call this with the event loop thread's id once that thread has been started, and with a
    /// default-constructed id once it has been joined. startStream() and stopStream() use it to
    /// decide whether to hand scheduler work to the event loop or to run it inline. Until it is
    /// set, both run inline, which is only correct while no event loop is running.
    void setEventLoopThread(std::thread::id eventLoopThreadId);

protected:
    ObRTSPClient(std::shared_ptr<const StreamProfile> profile, UsageEnvironment &env, char const *rtspURL, MutableFrameCallback callback, int verbosityLevel = 0, portNumBits tunnelOverHTTPPortNum = 0,
                 int socketNumToServer = -1);

private:
    /// Runs `command` on the live555 event loop thread when that thread is running and is not the
    /// caller, otherwise calls it inline. Returns once the command has been handed over, not once
    /// the server has answered; wait on commandCv_ for that.
    void dispatchToEventLoop(EventTriggerId trigger, void (ObRTSPClient::*command)());

    static void handleDescribeTrigger(void *clientData);
    static void handleTeardownTrigger(void *clientData);

    void DESCRIBE();
    // DESCRIBE command response, parse SDP, create media session
    static void cmdResponseHandlerDESCRIBE(RTSPClient *rtspClient, int resultCode, char *resultString);

    void SETUP();
    void setupNextSubsession();
    // SETUP command response, complete the creation of Subsession (create and bind sink)
    static void cmdResponseHandlerSETUP(RTSPClient *rtspClient, int resultCode, char *resultString);

    void PLAY();
    // PLAY command response, if the stream has a certain duration, create a scheduled task to close the stream
    static void cmdResponseHandlerPLAY(RTSPClient *rtspClient, int resultCode, char *resultString);

    void TEARDOWN();
    // TEARDOWN response
    static void cmdResponseHandlerTEARDOWN(RTSPClient *rtspClient, int resultCode, char *resultString);

    // Other event handler functions:
    static void subsessionAfterPlayingHandler(void *clientData);  // called when a stream's subsession (e.g., audio or video substream) ends
    static void subsessionByeHandler(void *clientData, char const *reason);
    // called when a RTCP "BYE" is received for a subsession
    static void streamStopEventHandler(void *clientData);
    // called at the end of a stream's expected duration (if the stream has not already signaled its end using a RTCP "BYE")

private:
    void clearSinks();

private:
    std::shared_ptr<const StreamProfile> profile_;
    MutableFrameCallback frameCallback_;

    std::string                 errorMsg_;
    CommandState                commandState_ = CMD_DONE;
    std::recursive_mutex        commandMutex_;
    std::condition_variable_any commandCv_;

    /// Written by the event loop thread and read by whichever thread called startStream().
    std::atomic<RTSPState> RTSPState_{ RTSP_DESCRIBE };

    std::atomic<std::thread::id> eventLoopThreadId_{};

    /// Zero until the scheduler grants a trigger; dispatchToEventLoop() then falls back to
    /// running commands inline.
    EventTriggerId describeTrigger_ = 0;
    EventTriggerId teardownTrigger_ = 0;

    MediaSession            *mediaSession_      = nullptr;
    MediaSubsession         *curSubsession_     = nullptr;
    MediaSubsessionIterator *curSubsessionIter_ = nullptr;

    double    duration_        = 0;
    TaskToken streamTimerTask_ = NULL;
};

}  // namespace libobsensor

