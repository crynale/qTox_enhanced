/*
    Copyright © 2013 by Maxim Biro <nurupo.contributions@gmail.com>
    Copyright © 2014-2019 by The qTox Project Contributors

    This file is part of qTox, a Qt-based graphical interface for Tox.

    qTox is libre software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    qTox is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with qTox.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "coreav.h"
#include "audio/iaudiosettings.h"
#include "core.h"
#include "src/model/friend.h"
#include "src/model/group.h"
#include "src/persistence/igroupsettings.h"
#include "src/video/corevideosource.h"
#include "src/video/videoframe.h"
#include "util/compatiblerecursivemutex.h"
#include "util/toxcoreerrorparser.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMutex>
#include <QThread>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <tox/toxav.h>

#define MINIAUDIO_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Watomic-implicit-seq-cst"
#pragma GCC diagnostic ignored "-Wbad-function-cast"
#pragma GCC diagnostic ignored "-Wswitch-enum"
#pragma GCC diagnostic ignored "-Wvector-conversion"
#pragma GCC diagnostic ignored "-Wtautological-type-limit-compare"
#pragma GCC diagnostic ignored "-Wmissing-declarations"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wnull-dereference"
#ifndef __APPLE__
#pragma GCC diagnostic ignored "-Wstringop-overflow="
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wall"
#include "miniaudio.h"
#pragma GCC diagnostic pop


#include <cassert>

#ifdef QTDEBUGMUTEXLOCKS
#define my_readlock() my_lock_aux(__FILE__, __LINE__, __func__)
void my_lock_aux(const char *file, int line, const char *function)
{
   printf("Thread %i is about to readlock mutex at [%s:%i:%s]\n", (int)pthread_self(), file, line, function);
}

#define my_unlockreadlock() my_unlock_aux(__FILE__, __LINE__, __func__)
void my_unlock_aux(const char *file, int line, const char *function)
{
  printf("Thread %i is about to UNlock readlock mutex at [%s:%i:%s]\n", (int)pthread_self(), file, line, function);
}

#define my_writelock() my_wlock_aux(__FILE__, __LINE__, __func__)
void my_wlock_aux(const char *file, int line, const char *function)
{
   printf("Thread %i is about to writelock mutex at [%s:%i:%s]\n", (int)pthread_self(), file, line, function);
}

#define my_unlockwritelock() my_wunlock_aux(__FILE__, __LINE__, __func__)
void my_wunlock_aux(const char *file, int line, const char *function)
{
  printf("Thread %i is about to UNlock writelock mutex at [%s:%i:%s]\n", (int)pthread_self(), file, line, function);
}
#else
#define my_readlock() do {} while(0)
#define my_unlockreadlock() do {} while(0)
#define my_writelock() do {} while(0)
#define my_unlockwritelock() do {} while(0)
#endif

/**
 * @fn void CoreAV::avInvite(uint32_t friendId, bool video)
 * @brief Sent when a friend calls us.
 * @param friendId Id of friend in call list.
 * @param video False if chat is audio only, true audio and video.
 *
 * @fn void CoreAV::avStart(uint32_t friendId, bool video)
 * @brief Sent when a call we initiated has started.
 * @param friendId Id of friend in call list.
 * @param video False if chat is audio only, true audio and video.
 *
 * @fn void CoreAV::avEnd(uint32_t friendId)
 * @brief Sent when a call was ended by the peer.
 * @param friendId Id of friend in call list.
 *
 * @var CoreAV::VIDEO_DEFAULT_BITRATE
 * @brief Picked at random by fair dice roll.
 */

/**
 * @var std::atomic_flag CoreAV::threadSwitchLock
 * @brief This flag is to be acquired before switching in a blocking way between the UI and CoreAV
 * thread.
 *
 * The CoreAV thread must have priority for the flag, other threads should back off or release it
 * quickly.
 * CoreAV needs to interface with three threads, the toxcore/Core thread that fires non-payload
 * toxav callbacks, the toxav/CoreAV thread that fires AV payload callbacks and manages
 * most of CoreAV's members, and the UI thread, which calls our [start/answer/cancel]Call functions
 * and which we call via signals.
 * When the UI calls us, we switch from the UI thread to the CoreAV thread to do the processing,
 * when toxcore fires a non-payload av callback, we do the processing in the CoreAV thread and then
 * switch to the UI thread to send it a signal. Both switches block both threads, so this would
 * deadlock.
 */

ma_resampler_config miniaudio_downsample_config;
ma_resampler miniaudio_downsample_resampler;
ma_resampler_config miniaudio_upsample_config;
ma_resampler miniaudio_upsample_resampler;

CoreAV::CoreAV(std::unique_ptr<ToxAV, ToxAVDeleter> toxav_, CompatibleRecursiveMutex& toxCoreLock,
               IAudioSettings& audioSettings_, IGroupSettings& groupSettings_, CameraSource& cameraSource_)
    : audio{nullptr}
    , toxav{std::move(toxav_)}
    , coreavThread{new QThread{this}}
    , iterateTimer{new QTimer{this}}
    , coreLock{toxCoreLock}
    , audioSettings{audioSettings_}
    , groupSettings{groupSettings_}
    , cameraSource{cameraSource_}
{
    assert(coreavThread);
    assert(iterateTimer);

    // filteraudio:X //
    assert(IAudioControl::AUDIO_SAMPLE_RATE == 48000);



    // ----------------------------------------------------------
    // Audio Resampling
    //
    miniaudio_downsample_config = ma_resampler_config_init(
        ma_format_s16,
        1,
        48000,
        16000,
        ma_resample_algorithm_linear);

    ma_result result1 = ma_resampler_init(&miniaudio_downsample_config, nullptr, &miniaudio_downsample_resampler);
    if (result1 != MA_SUCCESS) {
        qDebug() << "ma_resampler_init downsample -----> ERROR";
    }
    ma_resampler_set_rate(&miniaudio_downsample_resampler, 48000, 16000);

    miniaudio_upsample_config = ma_resampler_config_init(
        ma_format_s16,
        1,
        16000,
        48000,
        ma_resample_algorithm_linear);

    ma_result result2 = ma_resampler_init(&miniaudio_upsample_config, nullptr, &miniaudio_upsample_resampler);
    if (result2 != MA_SUCCESS) {
        qDebug() << "ma_resampler_init upsample -----> ERROR";
    }
    ma_resampler_set_rate(&miniaudio_upsample_resampler, 16000, 48000);
    //
    // ----------------------------------------------------------



    // ----------------------------------------------------------
    // Acoustic Echo Cancellation
    //
    webrtc_aecmInst = WebRtcAecm_Create();
    int32_t res1 = WebRtcAecm_Init(webrtc_aecmInst, (int32_t)(IAudioControl::AUDIO_SAMPLE_RATE / 3));
    qDebug() << "WebRtcAecm_Init ----->" << res1;
    // ----------------------------------------------------------
    // typedef struct {
    //     int16_t cngMode;            // AecmFalse, AecmTrue (default)
    //     int16_t echoMode;           // 0, 1, 2, 3 (default), 4
    // } AecmConfig;
    // ----------------------------------------------------------
    AecmConfig config;
    config.echoMode = AecmTrue;
    config.cngMode = audioSettings.getAecechomode();
    WebRtcAecm_set_config(webrtc_aecmInst, config);
    qDebug() << "WebRtcAecm_set_config ----->" << audioSettings.getAecechomode();
    //
    // ----------------------------------------------------------



    // ----------------------------------------------------------
    // Noise Suppression
    //
    nsxInst = WebRtcNsx_Create();
    int res2 = WebRtcNsx_Init(nsxInst, (int32_t)(IAudioControl::AUDIO_SAMPLE_RATE / 3));
    qDebug() << "WebRtcNsx_Init ----->" << res2;
    // ----------------------------------------------------------
    // mode          : 0: Mild, 1: Medium , 2: Aggressive, 3: more Aggressive
    // ----------------------------------------------------------
    int res3 = WebRtcNsx_set_policy(nsxInst, audioSettings.getAecechonsmode());
    qDebug() << "WebRtcNsx_set_policy: mode: " <<  audioSettings.getAecechonsmode() << "res :----->" << res3;
    //
    // ----------------------------------------------------------



    coreavThread->setObjectName("qTox CoreAV");
    moveToThread(coreavThread.get());

    connectCallbacks();

    iterateTimer->setSingleShot(true);

    connect(iterateTimer, &QTimer::timeout, this, &CoreAV::process);
    connect(coreavThread.get(), &QThread::finished, iterateTimer, &QTimer::stop);
    connect(coreavThread.get(), &QThread::started, this, &CoreAV::process);
}

void CoreAV::connectCallbacks()
{
    toxav_callback_call(toxav.get(), CoreAV::callCallback, this);
    toxav_callback_call_state(toxav.get(), CoreAV::stateCallback, this);
    toxav_callback_audio_bit_rate(toxav.get(), CoreAV::audioBitrateCallback, this);
    toxav_callback_video_bit_rate(toxav.get(), CoreAV::videoBitrateCallback, this);
    toxav_callback_audio_receive_frame(toxav.get(), CoreAV::audioFrameCallback, this);
    toxav_callback_video_receive_frame(toxav.get(), CoreAV::videoFrameCallback, this);
    toxav_callback_call_comm(toxav.get(), CoreAV::videoCommCallback, this);
}

/**
 * @brief Factory method for CoreAV
 * @param core pointer to the Tox instance
 * @return CoreAV instance on success, {} on failure
 */
CoreAV::CoreAVPtr CoreAV::makeCoreAV(Tox* core, CompatibleRecursiveMutex& toxCoreLock,
                                     IAudioSettings& audioSettings, IGroupSettings& groupSettings,
                                     CameraSource& cameraSource)
{
    Toxav_Err_New err;
    std::unique_ptr<ToxAV, ToxAVDeleter> toxav{toxav_new(core, &err)};
    switch (err) {
    case TOXAV_ERR_NEW_OK:
        break;
    case TOXAV_ERR_NEW_MALLOC:
        qCritical() << "Failed to allocate resources for ToxAV";
        return {};
    case TOXAV_ERR_NEW_MULTIPLE:
        qCritical() << "Attempted to create multiple ToxAV instances";
        return {};
    case TOXAV_ERR_NEW_NULL:
        qCritical() << "Unexpected NULL parameter";
        return {};
    }

    assert(toxav != nullptr);

    return CoreAVPtr{new CoreAV{std::move(toxav), toxCoreLock, audioSettings,
        groupSettings, cameraSource}};
}

/**
 * @brief Set the audio backend
 * @param audio The audio backend to use
 * @note This must be called before starting CoreAV and audio must outlive CoreAV
 */
void CoreAV::setAudio(IAudioControl& newAudio)
{
    audio.exchange(&newAudio);
}

/**
 * @brief Get the audio backend used
 * @return Pointer to the audio backend
 * @note This is needed only for the case CoreAV needs to restart and the restarting class doesn't
 * have access to the audio backend and wants to keep it the same.
 */
IAudioControl* CoreAV::getAudio()
{
    return audio;
}

CoreAV::~CoreAV()
{
    /* Gracefully leave calls and group calls to avoid deadlocks in destructor */
    for (const auto& call : calls) {
        cancelCall(call.first);
    }
    for (const auto& call : groupCalls) {
        leaveGroupCall(call.first);
    }

    assert(calls.empty());
    assert(groupCalls.empty());

    coreavThread->exit(0);
    coreavThread->wait();

    // filteraudio:X //
    WebRtcAecm_Free(webrtc_aecmInst);
    free(pcm_buf_out);

    ma_resampler_uninit(&miniaudio_downsample_resampler, nullptr);
    ma_resampler_uninit(&miniaudio_upsample_resampler, nullptr);
}

/**
 * @brief Starts the CoreAV main loop that calls toxav's main loop
 */
void CoreAV::start()
{
    coreavThread->start();
}

void CoreAV::process()
{
    assert(QThread::currentThread() == coreavThread.get());
    toxav_iterate(toxav.get());
    uint32_t interval = toxav_iteration_interval(toxav.get());
    if (interval <= 5)
    {
        interval = 10;
    }
    // qDebug() << "CoreAV:interval:" << interval;
    iterateTimer->start(interval);
}

/**
 * @brief Checks the call status for a Tox friend.
 * @param f the friend to check
 * @return true, if call is started for the friend, false otherwise
 */
bool CoreAV::isCallStarted(const Friend* f) const
{
    my_readlock();
    QReadLocker locker{&callsLock};
    bool ret = f && (calls.find(f->getId()) != calls.end());
    my_unlockreadlock();
    return ret;
}

/**
 * @brief Checks the call status for a Tox group.
 * @param g the group to check
 * @return true, if call is started for the group, false otherwise
 */
bool CoreAV::isCallStarted(const Group* g) const
{
    my_readlock();
    QReadLocker locker{&callsLock};
    bool ret = g && (groupCalls.find(g->getId()) != groupCalls.end());
    my_unlockreadlock();
    return ret;
}

/**
 * @brief Checks the call status for a Tox friend.
 * @param f the friend to check
 * @return true, if call is active for the friend, false otherwise
 */
bool CoreAV::isCallActive(const Friend* f) const
{
    my_readlock();
    QReadLocker locker{&callsLock};
    auto it = calls.find(f->getId());
    if (it == calls.end()) {
        my_unlockreadlock();
        return false;
    }
    bool ret = isCallStarted(f) && it->second->isActive();
    my_unlockreadlock();
    return ret;
}

/**
 * @brief Checks the call status for a Tox group.
 * @param g the group to check
 * @return true, if the call is active for the group, false otherwise
 */
bool CoreAV::isCallActive(const Group* g) const
{
    my_readlock();
    QReadLocker locker{&callsLock};
    auto it = groupCalls.find(g->getId());
    if (it == groupCalls.end()) {
        my_unlockreadlock();
        return false;
    }
    bool ret = isCallStarted(g) && it->second->isActive();
    my_unlockreadlock();
    return ret;
}

bool CoreAV::isCallVideoEnabled(const Friend* f) const
{
    my_readlock();
    QReadLocker locker{&callsLock};
    auto it = calls.find(f->getId());
    bool ret = isCallStarted(f) && it->second->getVideoEnabled();
    my_unlockreadlock();
    return ret;
}

bool CoreAV::answerCall(uint32_t friendNum, bool video)
{
    my_writelock();
    QWriteLocker locker{&callsLock};
    //**// QMutexLocker coreLocker{&coreLock};

    qDebug() << QString("Answering call %1").arg(friendNum);
    auto it = calls.find(friendNum);
    assert(it != calls.end());
    Toxav_Err_Answer err;

    const uint32_t videoBitrate = video ? VIDEO_DEFAULT_BITRATE : 0;
    if (toxav_answer(toxav.get(), friendNum, audioSettings.getAudioBitrate(),
                     videoBitrate, &err)) {
        it->second->setActive(true);

        if (audioSettings.getScreenVideoFPS() == 30) {
            qDebug() << "answerCall:setting HQ bitrate: 10000";
            toxav_option_set(toxav.get(), friendNum, TOXAV_ENCODER_VIDEO_BITRATE_AUTOSET, 0, NULL);
            toxav_option_set(toxav.get(), friendNum, TOXAV_ENCODER_VIDEO_MAX_BITRATE, 11000, NULL);
            toxav_option_set(toxav.get(), friendNum, TOXAV_ENCODER_VIDEO_MIN_BITRATE, 10000, NULL);
        } else if (audioSettings.getScreenVideoFPS() == 25) {
            qDebug() << "answerCall:setting HQ bitrate: 10000";
            toxav_option_set(toxav.get(), friendNum, TOXAV_ENCODER_VIDEO_BITRATE_AUTOSET, 0, NULL);
            toxav_option_set(toxav.get(), friendNum, TOXAV_ENCODER_VIDEO_MAX_BITRATE, 11000, NULL);
            toxav_option_set(toxav.get(), friendNum, TOXAV_ENCODER_VIDEO_MIN_BITRATE, 10000, NULL);
        } else if (audioSettings.getScreenVideoFPS() == 20) {
            qDebug() << "answerCall:setting HQ bitrate: AUTOSET";
            toxav_option_set(toxav.get(), friendNum, TOXAV_ENCODER_VIDEO_BITRATE_AUTOSET, 1, NULL);
            toxav_option_set(toxav.get(), friendNum, TOXAV_ENCODER_VIDEO_MAX_BITRATE, 180, NULL);
            toxav_option_set(toxav.get(), friendNum, TOXAV_ENCODER_VIDEO_MIN_BITRATE, 2700, NULL);
        }

        my_unlockwritelock();
        return true;
    } else {
        qWarning() << "Failed to answer call with error" << err;
        Toxav_Err_Call_Control controlErr;
        toxav_call_control(toxav.get(), friendNum, TOXAV_CALL_CONTROL_CANCEL, &controlErr);
        PARSE_ERR(controlErr);
        calls.erase(it);
        my_unlockwritelock();
        return false;
    }
}

bool CoreAV::startCall(uint32_t friendNum, bool video)
{
    my_writelock();
    QWriteLocker locker{&callsLock};
    //**// QMutexLocker coreLocker{&coreLock};

    qDebug() << QString("Starting call with %1").arg(friendNum);
    auto it = calls.find(friendNum);
    if (it != calls.end()) {
        qWarning() << QString("Can't start call with %1, we're already in this call!").arg(friendNum);
        my_unlockwritelock();
        return false;
    }

    uint32_t videoBitrate = video ? VIDEO_DEFAULT_BITRATE : 0;
    Toxav_Err_Call err;
    toxav_call(toxav.get(), friendNum, audioSettings.getAudioBitrate(), videoBitrate,
                    &err);
    if (!PARSE_ERR(err)) {
        my_unlockwritelock();
        return false;
    }

    // Audio backend must be set before making a call
    assert(audio != nullptr);
    ToxFriendCallPtr call = ToxFriendCallPtr(new ToxFriendCall(friendNum, video,
        *this, *audio, cameraSource));
    // Call object must be owned by this thread or there will be locking problems with Audio
    call->moveToThread(thread());
    assert(call != nullptr);
    calls.emplace(friendNum, std::move(call));

    if (audioSettings.getScreenVideoFPS() == 30) {
        qDebug() << "startCall:setting HQ bitrate: 10000";
        toxav_option_set(toxav.get(), friendNum, TOXAV_ENCODER_VIDEO_BITRATE_AUTOSET, 0, NULL);
        toxav_option_set(toxav.get(), friendNum, TOXAV_ENCODER_VIDEO_MAX_BITRATE, 11000, NULL);
        toxav_option_set(toxav.get(), friendNum, TOXAV_ENCODER_VIDEO_MIN_BITRATE, 10000, NULL);
    } else if (audioSettings.getScreenVideoFPS() == 25) {
        qDebug() << "startCall:setting HQ bitrate: 10000";
        toxav_option_set(toxav.get(), friendNum, TOXAV_ENCODER_VIDEO_BITRATE_AUTOSET, 0, NULL);
        toxav_option_set(toxav.get(), friendNum, TOXAV_ENCODER_VIDEO_MAX_BITRATE, 11000, NULL);
        toxav_option_set(toxav.get(), friendNum, TOXAV_ENCODER_VIDEO_MIN_BITRATE, 10000, NULL);
    } else if (audioSettings.getScreenVideoFPS() == 20) {
        qDebug() << "startCall:setting HQ bitrate: AUTOSET";
        toxav_option_set(toxav.get(), friendNum, TOXAV_ENCODER_VIDEO_BITRATE_AUTOSET, 1, NULL);
        toxav_option_set(toxav.get(), friendNum, TOXAV_ENCODER_VIDEO_MAX_BITRATE, 180, NULL);
        toxav_option_set(toxav.get(), friendNum, TOXAV_ENCODER_VIDEO_MIN_BITRATE, 2700, NULL);
    }

    my_unlockwritelock();
    return true;
}

bool CoreAV::cancelCall(uint32_t friendNum)
{
    my_writelock();
    QWriteLocker locker{&callsLock};
    //**// QMutexLocker coreLocker{&coreLock};

    qDebug() << QString("Cancelling call with %1").arg(friendNum);

    my_unlockwritelock();
    locker.unlock();
    Toxav_Err_Call_Control err;
    toxav_call_control(toxav.get(), friendNum, TOXAV_CALL_CONTROL_CANCEL, &err);
    my_writelock();
    locker.relock();

    if (!PARSE_ERR(err)) {
        my_unlockwritelock();
        return false;
    }

    calls.erase(friendNum);
    my_unlockwritelock();
    locker.unlock();

    emit avEnd(friendNum);
    return true;
}

void CoreAV::timeoutCall(uint32_t friendNum)
{
    my_writelock();
    QWriteLocker locker{&callsLock};

    if (!cancelCall(friendNum)) {
        qWarning() << QString("Failed to timeout call with %1").arg(friendNum);
        return;
    }
    qDebug() << "Call with friend" << friendNum << "timed out";
}

/**
 * @brief Send audio frame to a friend
 * @param callId Id of friend in call list.
 * @param pcm An array of audio samples (Pulse-code modulation).
 * @param samples Number of samples in this frame.
 * @param chans Number of audio channels.
 * @param rate Audio sampling rate used in this frame.
 * @return False only on error, but not if there's nothing to send.
 */
bool CoreAV::sendCallAudio(uint32_t callId, const int16_t* pcm, size_t samples, uint8_t chans,
                           uint32_t rate) const
{
#ifdef AV_TIMING_DEBUG
    qDebug() << "THREAD:sendCallAudio" <<  QThread::currentThread();
    static qint64 recurring_send_audio = QDateTime::currentDateTime().toMSecsSinceEpoch();
    qint64 old = recurring_send_audio;
    recurring_send_audio = QDateTime::currentDateTime().toMSecsSinceEpoch();
    qDebug() << "THREAD:sendCallAudio:recurr:" <<  (recurring_send_audio - old);
    QTime myTimer;
    myTimer.start();
#endif

/*
    if (!callsLock.tryLockForWrite()) {
        return false;
    } else {
        callsLock.unlock();
    }
*/
/*
    my_writelock();
    QWriteLocker locker{&callsLock};
*/

    auto it = calls.find(callId);
    if (it == calls.end()) {
        //my_unlockwritelock();
        return false;
    }

    ToxFriendCall const& call = *it->second;

    if (call.getMuteMic() || !call.isActive()
        || !(call.getState() & TOXAV_FRIEND_CALL_STATE_ACCEPTING_A)) {
        //my_unlockwritelock();
        return true;
    }

    // filteraudio:X //
    static int current_echo_latency = 80; // HINT: static, to remember the value on the next calls
    static int current_aec_echo_mode = 0; // HINT: static, to remember the value on the next calls
    static int current_aec_ns_echo_mode = 0; // HINT: static, to remember the value on the next calls
    // qDebug() << "filter_audio recorded audio: chans:" << chans << " rate:" << rate;
    if ((chans == 1) && (rate == IAudioControl::AUDIO_SAMPLE_RATE)) {
        if (audioSettings.getEchoCancellation()) {
            int new_echo_latency = audioSettings.getEchoLatency();
            if (new_echo_latency != current_echo_latency) {
                current_echo_latency = new_echo_latency;
                int16_t filterLatency = current_echo_latency;
                qDebug() << "Setting filter delay to: " << filterLatency << "ms";
            }

            int new_aec_echo_mode = audioSettings.getAecechomode();
            if (new_aec_echo_mode != current_aec_echo_mode) {
                current_aec_echo_mode = new_aec_echo_mode;
                qDebug() << "Setting AEC Mode to: " << current_aec_echo_mode;

                AecmConfig config;
                config.echoMode = AecmTrue;
                config.cngMode = current_aec_echo_mode;
                WebRtcAecm_set_config(webrtc_aecmInst, config);
                qDebug() << "WebRtcAecm_set_config ----->" << audioSettings.getAecechomode();
            }

            int new_aec_ns_echo_mode = audioSettings.getAecechonsmode();
            if (new_aec_ns_echo_mode != current_aec_ns_echo_mode) {
                current_aec_ns_echo_mode = new_aec_ns_echo_mode;
                qDebug() << "Setting AEC NS Mode to: " << new_aec_ns_echo_mode;
                int res3 = WebRtcNsx_set_policy(nsxInst, new_aec_ns_echo_mode);
                qDebug() << "WebRtcNsx_set_policy: mode: " <<  new_aec_ns_echo_mode << "res :----->" << res3;
            }

            const int split_factor = (IAudioControl::AUDIO_FRAME_DURATION / 10);
            const int sample_count_split = samples / split_factor;

            if (pcm_buf_out_samples != samples) {
                free(pcm_buf_out);
                pcm_buf_out = nullptr;
            }
            if (pcm_buf_out == nullptr) {
                pcm_buf_out = (int16_t *)calloc(1, 2 * samples);
                pcm_buf_out_samples = samples;
            }

            // downsample to 16khz
            int16_t *pcm_buf_resampled = (int16_t *)calloc(1, sizeof(int16_t) * (samples / 3));
            downsample_48000_to_16000_basic(pcm, pcm_buf_resampled, samples);
            // printf("WebRtcAecm_Process:samples=%d split_factor=%d sample_count_split=%d\n",
            //         (int32_t)samples, split_factor, sample_count_split);
            int16_t *pcm_buf_out_resampled = (int16_t *)calloc(1, sizeof(int16_t) * (samples / 3));
            int16_t *pcm_buf_filtered_out_resampled = (int16_t *)calloc(1, sizeof(int16_t) * (samples / 3));

            for (int x=0;x<split_factor;x++)
            {
                short const *const tmp1[] = { pcm_buf_resampled + (x * (sample_count_split / 3)), 0 };
                short *const tmp2[] = { pcm_buf_filtered_out_resampled + (x * (sample_count_split / 3)), 0 };
                WebRtcNsx_Process(nsxInst,
                                tmp1,
                                1,
                                tmp2);

                aec_mutex.lock();
                int32_t res = WebRtcAecm_Process(
                        webrtc_aecmInst,
                        const_cast<int16_t *>(pcm_buf_resampled + (x * (sample_count_split / 3))),
                        const_cast<int16_t *>(pcm_buf_filtered_out_resampled + (x * (sample_count_split / 3))),
                        pcm_buf_out_resampled + (x * (sample_count_split / 3)),
                        (sample_count_split / 3),
                        current_echo_latency + IAudioControl::AUDIO_FRAME_DURATION
                        );
                aec_mutex.unlock();
                std::ignore = res;
                // printf("WebRtcAecm_Process:%d\n", res);
            }

            // upsample back to 48khz
            upsample_16000_to_48000_basic(pcm_buf_out_resampled, pcm_buf_out, samples / 3);
            free(pcm_buf_filtered_out_resampled);
            free(pcm_buf_resampled);
            free(pcm_buf_out_resampled);

            memcpy(const_cast<int16_t *>(pcm), pcm_buf_out, 2 * samples);
        }
    }

    // TOXAV_ERR_SEND_FRAME_SYNC means toxav failed to lock, retry 5 times in this case
    Toxav_Err_Send_Frame err;
    int retries = 0;
    do {
        if (!toxav_audio_send_frame(toxav.get(), callId, pcm, samples, chans, rate, &err)) {
            if (err == TOXAV_ERR_SEND_FRAME_SYNC) {
                ++retries;
                QThread::usleep(500);
            } else {
                qDebug() << "toxav_audio_send_frame error: " << err;
            }
        }
    } while (err == TOXAV_ERR_SEND_FRAME_SYNC && retries < 3);
    if (err == TOXAV_ERR_SEND_FRAME_SYNC) {
        qDebug() << "toxav_audio_send_frame error: Lock busy, dropping frame";
    }

    //my_unlockwritelock();
#ifdef AV_TIMING_DEBUG
    qDebug() << "THREAD:sendCallAudio:duration:" << myTimer.elapsed();
#endif
    return true;
}

void CoreAV::sendCallVideo(uint32_t callId, std::shared_ptr<VideoFrame> vframe)
{
#ifdef AV_TIMING_DEBUG
    qDebug() << "THREAD:sendCallVideo" <<  QThread::currentThread();
    static qint64 recurring_send_video = QDateTime::currentDateTime().toMSecsSinceEpoch();
    qint64 old = recurring_send_video;
    recurring_send_video = QDateTime::currentDateTime().toMSecsSinceEpoch();
    qDebug() << "THREAD:sendCallVideo:recurr:" <<  (recurring_send_video - old);
    QTime myTimer;
    myTimer.start();
#endif

    if (!callsLock.tryLockForRead()) {
        qDebug() << "sendCallVideo:tryLockForRead failed";
        return;
    } else {
        callsLock.unlock();
    }
    my_readlock();
    QReadLocker locker{&callsLock};

    // We might be running in the FFmpeg thread and holding the CameraSource lock
    // So be careful not to deadlock with anything while toxav locks in toxav_video_send_frame
    auto it = calls.find(callId);
    if (it == calls.end()) {
        my_unlockreadlock();
        return;
    }

    ToxFriendCall& call = *it->second;

    if (!call.getVideoEnabled() || !call.isActive()
        || !(call.getState() & TOXAV_FRIEND_CALL_STATE_ACCEPTING_V)) {
        my_unlockreadlock();
        return;
    }

    if (call.getNullVideoBitrate()) {
        qDebug() << "Restarting video stream to friend" << callId;
        //**// QMutexLocker coreLocker{&coreLock};
        Toxav_Err_Bit_Rate_Set err;
        toxav_video_set_bit_rate(toxav.get(), callId, VIDEO_DEFAULT_BITRATE, &err);
        if (!PARSE_ERR(err)) {
            my_unlockreadlock();
            return;
        }
        call.setNullVideoBitrate(false);
    }

    QRect vsize = vframe->getSourceDimensions();
    QSize new_size = QSize(vsize.width(), vsize.height());
    // 3840x2160 -> 4K resolution
    if ((vsize.width() > 1920) || (vsize.height() > 1080)) {
        new_size = QSize(1920, 1080);
    }
    ToxYUVFrame frame = vframe->toToxYUVFrame(new_size);

    if (!frame) {
        my_unlockreadlock();
        return;
    }

    // TOXAV_ERR_SEND_FRAME_SYNC means toxav failed to lock, retry 5 times in this case
    // We don't want to be dropping iframes because of some lock held by toxav_iterate
    Toxav_Err_Send_Frame err;
    if (!toxav_video_send_frame(toxav.get(), callId, frame.width, frame.height, frame.y,
                                frame.u, frame.v, &err)) {
            qDebug() << "toxav_video_send_frame error: " << err;
    }

    my_unlockreadlock();
#ifdef AV_TIMING_DEBUG
    qDebug() << "THREAD:sendCallVideo:duration:" << myTimer.elapsed();
#endif
}

/**
 * @brief Toggles the mute state of the call's input (microphone).
 * @param f The friend assigned to the call
 */
void CoreAV::toggleMuteCallInput(const Friend* f)
{
    my_writelock();
    QWriteLocker locker{&callsLock};

    auto it = calls.find(f->getId());
    if (f && (it != calls.end())) {
        ToxCall& call = *it->second;
        call.setMuteMic(!call.getMuteMic());
    }
}

/**
 * @brief Toggles the mute state of the call's output (speaker).
 * @param f The friend assigned to the call
 */
void CoreAV::toggleMuteCallOutput(const Friend* f)
{
    my_writelock();
    QWriteLocker locker{&callsLock};

    auto it = calls.find(f->getId());
    if (f && (it != calls.end())) {
        ToxCall& call = *it->second;
        call.setMuteVol(!call.getMuteVol());
    }
}

/**
 * @brief Called from Tox API when group call receives audio data.
 *
 * @param[in] tox          the Tox object
 * @param[in] group        the group number
 * @param[in] peer         the peer number
 * @param[in] data         the audio data to playback
 * @param[in] samples      the audio samples
 * @param[in] channels     the audio channels
 * @param[in] sample_rate  the audio sample rate
 * @param[in] core         the qTox Core class
 */
void CoreAV::groupCallCallback(void* tox, uint32_t group, uint32_t peer, const int16_t* data,
                               unsigned samples, uint8_t channels, uint32_t sample_rate, void* core)
{
    /*
     * Currently group call audio decoding is handled in the Tox thread by c-toxcore,
     * so we can be sure that this function is always called from the Core thread.
     * To change this, an API change in c-toxcore is needed and this function probably must be
     * changed.
     * See https://github.com/TokTok/c-toxcore/issues/1364 for details.
     */

    std::ignore = tox;
    Core* c = static_cast<Core*>(core);
    CoreAV* cav = c->getAv();

    my_readlock();
    QReadLocker locker{&cav->callsLock};

    const ToxPk peerPk = c->getGroupPeerPk(group, peer);
    // don't play the audio if it comes from a muted peer
    if (cav->groupSettings.getBlackList().contains(peerPk.toString())) {
        return;
    }

    emit c->groupPeerAudioPlaying(group, peerPk);

    auto it = cav->groupCalls.find(group);
    if (it == cav->groupCalls.end()) {
        return;
    }

    ToxGroupCall& call = *it->second;

    if (call.getMuteVol() || !call.isActive()) {
        return;
    }

    call.playAudioBuffer(peerPk, data, samples, channels, sample_rate);
}

/**
 * @brief Called from core to make sure the source for that peer is invalidated when they leave.
 * @param group Group Index
 * @param peer Peer Index
 */
void CoreAV::invalidateGroupCallPeerSource(const Group& group, ToxPk peerPk)
{
    my_writelock();
    QWriteLocker locker{&callsLock};

    auto it = groupCalls.find(group.getId());
    if (it == groupCalls.end()) {
        return;
    }
    it->second->removePeer(peerPk);
}

/**
 * @brief Get a call's video source.
 * @param friendNum Id of friend in call list.
 * @return Video surface to show
 */
VideoSource* CoreAV::getVideoSourceFromCall(int friendNum) const
{
    my_readlock();
    QReadLocker locker{&callsLock};

    auto it = calls.find(friendNum);
    if (it == calls.end()) {
        qWarning() << "CoreAV::getVideoSourceFromCall: No such call, possibly cancelled";
        my_unlockreadlock();
        return nullptr;
    }

    VideoSource* ret = it->second->getVideoSource();
    my_unlockreadlock();
    return ret;
}

/**
 * @brief Starts a call in an existing AV groupchat.
 * @note Call from the GUI thread.
 * @param groupId Id of group to join
 */
void CoreAV::joinGroupCall(const Group& group)
{
    my_writelock();
    QWriteLocker locker{&callsLock};

    qDebug() << QString("Joining group call %1").arg(group.getId());

    // Audio backend must be set before starting a call
    assert(audio != nullptr);

    ToxGroupCallPtr groupcall = ToxGroupCallPtr(new ToxGroupCall{group, *this, *audio});
    // Call Objects must be owned by CoreAV or there will be locking problems with Audio
    groupcall->moveToThread(thread());
    assert(groupcall != nullptr);
    auto ret = groupCalls.emplace(group.getId(), std::move(groupcall));
    if (ret.second == false) {
        qWarning() << "This group call already exists, not joining!";
        return;
    }
    ret.first->second->setActive(true);
}

/**
 * @brief Will not leave the group, just stop the call.
 * @note Call from the GUI thread.
 * @param groupId Id of group to leave
 */
void CoreAV::leaveGroupCall(int groupNum)
{
    my_writelock();
    QWriteLocker locker{&callsLock};

    qDebug() << QString("Leaving group call %1").arg(groupNum);

    groupCalls.erase(groupNum);
}

bool CoreAV::sendGroupCallAudio(int groupNum, const int16_t* pcm, size_t samples, uint8_t chans,
                                uint32_t rate) const
{
    my_readlock();
    QReadLocker locker{&callsLock};

    std::map<int, ToxGroupCallPtr>::const_iterator it = groupCalls.find(groupNum);
    if (it == groupCalls.end()) {
        my_unlockreadlock();
        return false;
    }

    if (!it->second->isActive() || it->second->getMuteMic()) {
        my_unlockreadlock();
        return true;
    }

    if (toxav_group_send_audio(toxav_get_tox(toxav.get()), groupNum, pcm, samples, chans, rate) != 0)
        qDebug() << "toxav_group_send_audio error";

    my_unlockreadlock();
    return true;
}

/**
 * @brief Mutes or unmutes the group call's input (microphone).
 * @param g The group
 * @param mute True to mute, false to unmute
 */
void CoreAV::muteCallInput(const Group* g, bool mute)
{
    my_writelock();
    QWriteLocker locker{&callsLock};

    auto it = groupCalls.find(g->getId());
    if (g && (it != groupCalls.end())) {
        it->second->setMuteMic(mute);
    }
}

/**
 * @brief Mutes or unmutes the group call's output (speaker).
 * @param g The group
 * @param mute True to mute, false to unmute
 */
void CoreAV::muteCallOutput(const Group* g, bool mute)
{
    my_writelock();
    QWriteLocker locker{&callsLock};

    auto it = groupCalls.find(g->getId());
    if (g && (it != groupCalls.end())) {
        it->second->setMuteVol(mute);
    }
}

/**
 * @brief Returns the group calls input (microphone) state.
 * @param groupId The group id to check
 * @return true when muted, false otherwise
 */
bool CoreAV::isGroupCallInputMuted(const Group* g) const
{
    my_readlock();
    QReadLocker locker{&callsLock};

    if (!g) {
        my_unlockreadlock();
        return false;
    }

    const uint32_t groupId = g->getId();
    auto it = groupCalls.find(groupId);
    bool ret = (it != groupCalls.end()) && it->second->getMuteMic();
    my_unlockreadlock();
    return ret;
}

/**
 * @brief Returns the group calls output (speaker) state.
 * @param groupId The group id to check
 * @return true when muted, false otherwise
 */
bool CoreAV::isGroupCallOutputMuted(const Group* g) const
{
    my_readlock();
    QReadLocker locker{&callsLock};

    if (!g) {
        my_unlockreadlock();
        return false;
    }

    const uint32_t groupId = g->getId();
    auto it = groupCalls.find(groupId);
    bool ret = (it != groupCalls.end()) && it->second->getMuteVol();
    my_unlockreadlock();
    return ret;
}

/**
 * @brief Returns the calls input (microphone) mute state.
 * @param f The friend to check
 * @return true when muted, false otherwise
 */
bool CoreAV::isCallInputMuted(const Friend* f) const
{
    my_readlock();
    QReadLocker locker{&callsLock};

    if (!f) {
        my_unlockreadlock();
        return false;
    }
    const uint32_t friendId = f->getId();
    auto it = calls.find(friendId);
    bool ret = (it != calls.end()) && it->second->getMuteMic();
    my_unlockreadlock();
    return ret;
}

/**
 * @brief Returns the calls output (speaker) mute state.
 * @param friendId The friend to check
 * @return true when muted, false otherwise
 */
bool CoreAV::isCallOutputMuted(const Friend* f) const
{
    my_readlock();
    QReadLocker locker{&callsLock};

    if (!f) {
        my_unlockreadlock();
        return false;
    }
    const uint32_t friendId = f->getId();
    auto it = calls.find(friendId);
    bool ret = (it != calls.end()) && it->second->getMuteVol();
    my_unlockreadlock();
    return ret;
}

/**
 * @brief Signal to all peers that we're not sending video anymore.
 * @note The next frame sent cancels this.
 */
void CoreAV::sendNoVideo()
{
    my_writelock();
    QWriteLocker locker{&callsLock};

    // We don't change the audio bitrate, but we signal that we're not sending video anymore
    qDebug() << "CoreAV: Signaling end of video sending";
    for (auto& kv : calls) {
        ToxFriendCall& call = *kv.second;
        Toxav_Err_Bit_Rate_Set err;
        toxav_video_set_bit_rate(toxav.get(), kv.first, 0, &err);
        if (!PARSE_ERR(err)) {
            continue;
        }
        call.setNullVideoBitrate(true);
    }
}

void CoreAV::callCallback(ToxAV* toxav, uint32_t friendNum, bool audio, bool video, void* vSelf)
{
    CoreAV* self = static_cast<CoreAV*>(vSelf);

    my_writelock();
    QWriteLocker locker{&self->callsLock};

    // Audio backend must be set before receiving a call
    assert(self->audio != nullptr);
    ToxFriendCallPtr call = ToxFriendCallPtr(new ToxFriendCall{friendNum, video,
        *self, *self->audio, self->cameraSource});
    // Call object must be owned by CoreAV thread or there will be locking problems with Audio
    call->moveToThread(self->thread());
    assert(call != nullptr);

    auto it = self->calls.emplace(friendNum, std::move(call));
    if (it.second == false) {
        qWarning() << QString("Rejecting call invite from %1, we're already in that call!").arg(friendNum);
        Toxav_Err_Call_Control err;
        toxav_call_control(toxav, friendNum, TOXAV_CALL_CONTROL_CANCEL, &err);
        PARSE_ERR(err);
        return;
    }
    qDebug() << QString("Received call invite from %1").arg(friendNum);

    // We don't get a state callback when answering, so fill the state ourselves in advance
    int state = 0;
    if (audio)
        state |= TOXAV_FRIEND_CALL_STATE_SENDING_A | TOXAV_FRIEND_CALL_STATE_ACCEPTING_A;
    if (video)
        state |= TOXAV_FRIEND_CALL_STATE_SENDING_V | TOXAV_FRIEND_CALL_STATE_ACCEPTING_V;
    it.first->second->setState(static_cast<TOXAV_FRIEND_CALL_STATE>(state));

    // Must explicitely unlock, because a deadlock can happen via ChatForm/Audio
    locker.unlock();

    emit self->avInvite(friendNum, video);
}

void CoreAV::stateCallback(ToxAV* toxav, uint32_t friendNum, uint32_t state, void* vSelf)
{
    std::ignore = toxav;
    CoreAV* self = static_cast<CoreAV*>(vSelf);

    // we must unlock this lock before emitting any signals
    my_writelock();
    QWriteLocker locker{&self->callsLock};

    auto it = self->calls.find(friendNum);
    if (it == self->calls.end()) {
        qWarning() << QString("stateCallback called, but call %1 is already dead").arg(friendNum);
        my_unlockwritelock();
        return;
    }

    ToxFriendCall& call = *it->second;

    if (state & TOXAV_FRIEND_CALL_STATE_ERROR) {
        qWarning() << "Call with friend" << friendNum << "died of unnatural causes!";
        self->calls.erase(friendNum);
        my_unlockwritelock();
        locker.unlock();
        emit self->avEnd(friendNum, true);
    } else if (state & TOXAV_FRIEND_CALL_STATE_FINISHED) {
        qDebug() << "Call with friend" << friendNum << "finished quietly";
        self->calls.erase(friendNum);
        my_unlockwritelock();
        locker.unlock();
        emit self->avEnd(friendNum);
    } else {
        // If our state was null, we started the call and were still ringing
        if (!call.getState() && state) {
            call.setActive(true);
            bool videoEnabled = call.getVideoEnabled();
            call.setState(static_cast<TOXAV_FRIEND_CALL_STATE>(state));
            my_unlockwritelock();
            locker.unlock();
            emit self->avStart(friendNum, videoEnabled);
        } else if ((call.getState() & TOXAV_FRIEND_CALL_STATE_SENDING_V)
                   && !(state & TOXAV_FRIEND_CALL_STATE_SENDING_V)) {
            qDebug() << "Friend" << friendNum << "stopped sending video";
            if (call.getVideoSource()) {
                call.getVideoSource()->stopSource();
            }

            call.setState(static_cast<TOXAV_FRIEND_CALL_STATE>(state));
        } else if (!(call.getState() & TOXAV_FRIEND_CALL_STATE_SENDING_V)
                   && (state & TOXAV_FRIEND_CALL_STATE_SENDING_V)) {
            // Workaround toxav sometimes firing callbacks for "send last frame" -> "stop sending
            // video"
            // out of orders (even though they were sent in order by the other end).
            // We simply stop the videoSource from emitting anything while the other end says it's
            // not sending
            if (call.getVideoSource()) {
                call.getVideoSource()->restartSource();
            }

            call.setState(static_cast<TOXAV_FRIEND_CALL_STATE>(state));
        }
    }

    // maybe??
    my_unlockwritelock();
}

// This is only a dummy implementation for now
void CoreAV::bitrateCallback(ToxAV* toxav, uint32_t friendNum, uint32_t arate, uint32_t vrate,
                             void* vSelf)
{
    CoreAV* self = static_cast<CoreAV*>(vSelf);
    std::ignore = self;
    std::ignore = toxav;

    qDebug() << "Recommended bitrate with" << friendNum << " is now " << arate << "/" << vrate
             << ", ignoring it";
}

// This is only a dummy implementation for now
void CoreAV::audioBitrateCallback(ToxAV* toxav, uint32_t friendNum, uint32_t rate, void* vSelf)
{
    CoreAV* self = static_cast<CoreAV*>(vSelf);
    std::ignore = self;
    std::ignore = toxav;

    qDebug() << "Recommended audio bitrate with" << friendNum << " is now " << rate << ", ignoring it";
}

// This is only a dummy implementation for now
void CoreAV::videoBitrateCallback(ToxAV* toxav, uint32_t friendNum, uint32_t rate, void* vSelf)
{
    CoreAV* self = static_cast<CoreAV*>(vSelf);
    std::ignore = self;
    std::ignore = toxav;

    qDebug() << "Recommended video bitrate with" << friendNum << " is now " << rate << ", ignoring it";
}

void CoreAV::audioFrameCallback(ToxAV* toxAV, uint32_t friendNum, const int16_t* pcm, size_t sampleCount,
                                uint8_t channels, uint32_t samplingRate, void* vSelf)
{
    std::ignore = toxAV;
    CoreAV* self = static_cast<CoreAV*>(vSelf);
    // This callback should come from the CoreAV thread
    assert(QThread::currentThread() == self->coreavThread.get());
    // QReadLocker locker{&self->callsLock};

    auto it = self->calls.find(friendNum);
    if (it == self->calls.end()) {
        return;
    }

    ToxFriendCall& call = *it->second;

    if (call.getMuteVol()) {
        return;
    }

    // filteraudio:X //
    // qDebug() << "filter_audio playback audio: chans:" << channels << "rate:" << samplingRate << "sampleCount:" << sampleCount;
    if (    (channels == 1)
            && (samplingRate == IAudioControl::AUDIO_SAMPLE_RATE)
            && ((sampleCount == 1920) || (sampleCount == 2880))
        ) {
        if (self->audioSettings.getEchoCancellation()) {
            const int audio_frame_in_ms = (sampleCount * 1000) / samplingRate;
            // HINT: we allow 40ms and 60ms sound incoming @48kHz Mono
            // printf("WebRtcAecm_BufferFarend:audio_frame_in_ms:%d\n", audio_frame_in_ms);
            if (audio_frame_in_ms >= 10)
            {
                // downsample to 16khz
                int16_t *pcm_buf_resampled = (int16_t *)calloc(1, sizeof(int16_t) * (sampleCount / 3));
                downsample_48000_to_16000_basic(pcm, pcm_buf_resampled, sampleCount);

                const int split_factor = (audio_frame_in_ms / 10);
                const int sample_count_split = sampleCount / split_factor;
                // printf("WebRtcAecm_BufferFarend:audio_frame_in_ms:split_factor=%d sampleCount=%d sample_count_split=%d\n",
                //         split_factor, (int32_t)sampleCount, sample_count_split);
                for (int x=0;x<split_factor;x++)
                {
                    self->aec_mutex.lock();
                    int32_t res = WebRtcAecm_BufferFarend(
                                    self->webrtc_aecmInst,
                                    pcm_buf_resampled + (x * (sample_count_split / 3)),
                                    (int16_t)(sample_count_split / 3)
                                    );
                    self->aec_mutex.unlock();
                    std::ignore = res;
                    // printf("WebRtcAecm_BufferFarend:#%d %d\n", x, res);
                }

                free(pcm_buf_resampled);
            }
        }
    }

    call.playAudioBuffer(pcm, sampleCount, channels, samplingRate);
}

void CoreAV::videoFrameCallback(ToxAV* toxAV, uint32_t friendNum, uint16_t w, uint16_t h,
                                const uint8_t* y, const uint8_t* u, const uint8_t* v,
                                int32_t ystride, int32_t ustride, int32_t vstride, void* vSelf)
{
    std::ignore = toxAV;
    auto self = static_cast<CoreAV*>(vSelf);
    // This callback should come from the CoreAV thread
    assert(QThread::currentThread() == self->coreavThread.get());
    QReadLocker locker{&self->callsLock};

    auto it = self->calls.find(friendNum);
    if (it == self->calls.end()) {
        return;
    }

    CoreVideoSource* videoSource = it->second->getVideoSource();
    if (!videoSource) {
        return;
    }

    vpx_image frame;
    frame.d_h = h;
    frame.d_w = w;
    frame.planes[0] = const_cast<uint8_t*>(y);
    frame.planes[1] = const_cast<uint8_t*>(u);
    frame.planes[2] = const_cast<uint8_t*>(v);
    frame.stride[0] = ystride;
    frame.stride[1] = ustride;
    frame.stride[2] = vstride;

    videoSource->pushFrame(&frame);
}

void CoreAV::videoCommCallback(ToxAV* toxAV, uint32_t friend_number, TOXAV_CALL_COMM_INFO comm_value,
                                int64_t comm_number, void *vSelf)
{
    std::ignore = comm_number;
    auto self = static_cast<CoreAV*>(vSelf);

    if (comm_value == TOXAV_CALL_COMM_ENCODER_CURRENT_BITRATE) {
        if (self->audioSettings.getScreenVideoFPS() == 30) {
            toxav_option_set(toxAV, friend_number, TOXAV_ENCODER_VIDEO_BITRATE_AUTOSET, 0, NULL);
            toxav_option_set(toxAV, friend_number, TOXAV_ENCODER_VIDEO_MAX_BITRATE, 11000, NULL);
            toxav_option_set(toxAV, friend_number, TOXAV_ENCODER_VIDEO_MIN_BITRATE, 10000, NULL);
        } else if (self->audioSettings.getScreenVideoFPS() == 25) {
            toxav_option_set(toxAV, friend_number, TOXAV_ENCODER_VIDEO_BITRATE_AUTOSET, 0, NULL);
            toxav_option_set(toxAV, friend_number, TOXAV_ENCODER_VIDEO_MAX_BITRATE, 11000, NULL);
            toxav_option_set(toxAV, friend_number, TOXAV_ENCODER_VIDEO_MIN_BITRATE, 10000, NULL);
        } else if (self->audioSettings.getScreenVideoFPS() == 20) {
            toxav_option_set(toxAV, friend_number, TOXAV_ENCODER_VIDEO_BITRATE_AUTOSET, 1, NULL);
            toxav_option_set(toxAV, friend_number, TOXAV_ENCODER_VIDEO_MAX_BITRATE, 180, NULL);
            toxav_option_set(toxAV, friend_number, TOXAV_ENCODER_VIDEO_MIN_BITRATE, 2700, NULL);
        }
    }
}

int32_t CoreAV::upsample_16000_to_48000_basic(const int16_t* in, int16_t *out, int32_t sample_count)
{
    if (sample_count < 1)
    {
        return -1;
    }

    ma_uint64 frameCountIn  = sample_count;
    ma_uint64 frameCountOut = sample_count * 3;
    ma_result result = ma_resampler_process_pcm_frames(&miniaudio_upsample_resampler, in, &frameCountIn, out, &frameCountOut);
    if (result != MA_SUCCESS) {
        qDebug() << "upsample_16000_to_48000_basic:error!!";
    }

#if 0
    int16_t tmp1;
    int16_t tmp2;
    int16_t v1;
    int16_t v2;
    for (int i=0;i<sample_count;i++)
    {
        tmp1 = *(in);
        tmp2 = *(in + 1);
        v1 =   (int16_t)((float)(tmp2 - tmp1) / 3.0f);
        v2 =   (int16_t)(((float)(tmp2 - tmp1) * 2.0f) / 3.0f);
        *out = tmp1;
        out++;
        *out = tmp1 + v1;
        out++;
        *out = tmp1 + v2;
        out++;
        in++;
    }
#endif

    return 0;
}

int32_t CoreAV::downsample_48000_to_16000_basic(const int16_t* in, int16_t *out, int32_t sample_count)
{
    if (sample_count < 3)
    {
        return -1;
    }

    ma_uint64 frameCountIn  = sample_count;
    ma_uint64 frameCountOut = sample_count / 3;
    ma_result result = ma_resampler_process_pcm_frames(&miniaudio_downsample_resampler, in, &frameCountIn, out, &frameCountOut);
    if (result != MA_SUCCESS) {
        qDebug() << "downsample_48000_to_16000_basic:error!!";
    }

#if 0
    for (int i=0;i<(sample_count / 3);i++)
    {
        *out = *(in + 0);
        out++;
        in = in + 3;
    }
#endif

    return 0;
}
