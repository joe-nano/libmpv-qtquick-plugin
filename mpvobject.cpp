#include "mpvobject.h"

#include <QDebug>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QQuickWindow>
#include <utility>
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <QGuiApplication>
#include <QX11Info>
#endif

namespace {

void wakeup(void *ctx) {
    // This callback is invoked from any mpv thread (but possibly also
    // recursively from a thread that is calling the mpv API). Just notify
    // the Qt GUI thread to wake up (so that it can process events with
    // mpv_wait_event()), and return as quickly as possible.
    QMetaObject::invokeMethod(static_cast<MpvObject *>(ctx), "hasMpvEvents",
                              Qt::QueuedConnection);
}

void on_mpv_redraw(void *ctx) { MpvObject::on_update(ctx); }

void *get_proc_address_mpv(void *ctx, const char *name) {
    Q_UNUSED(ctx)
    QOpenGLContext *glctx = QOpenGLContext::currentContext();
    if (glctx == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void *>(glctx->getProcAddress(QByteArray(name)));
}

} // namespace

class MpvRenderer : public QQuickFramebufferObject::Renderer {
    Q_DISABLE_COPY_MOVE(MpvRenderer)

public:
    MpvRenderer(MpvObject *mpvObject) : m_mpvObject(mpvObject) {
        Q_ASSERT(m_mpvObject != nullptr);
    }
    ~MpvRenderer() override = default;

    // This function is called when a new FBO is needed.
    // This happens on the initial frame.
    QOpenGLFramebufferObject *
    createFramebufferObject(const QSize &size) override {
        // init mpv_gl:
        if (m_mpvObject->mpv_gl == nullptr) {
            mpv_opengl_init_params gl_init_params{get_proc_address_mpv, nullptr,
                                                  nullptr};
            mpv_render_param params[]{
                {MPV_RENDER_PARAM_API_TYPE,
                 const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
                {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
                {MPV_RENDER_PARAM_INVALID, nullptr},
                {MPV_RENDER_PARAM_INVALID, nullptr}};
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
            if (QGuiApplication::platformName().contains("xcb",
                                                         Qt::CaseInsensitive)) {
                params[2].type = MPV_RENDER_PARAM_X11_DISPLAY;
                params[2].data = QX11Info::display();
            }
#endif

            const int mpvGLInitResult = mpv_render_context_create(
                &m_mpvObject->mpv_gl, m_mpvObject->mpv, params);
            Q_ASSERT(mpvGLInitResult >= 0);
            mpv_render_context_set_update_callback(m_mpvObject->mpv_gl,
                                                   on_mpv_redraw, m_mpvObject);

            QMetaObject::invokeMethod(m_mpvObject, "initFinished");
        }

        return QQuickFramebufferObject::Renderer::createFramebufferObject(size);
    }

    void render() override {
        m_mpvObject->window()->resetOpenGLState();

        QOpenGLFramebufferObject *fbo = framebufferObject();
        mpv_opengl_fbo mpfbo;
        mpfbo.fbo = static_cast<int>(fbo->handle());
        mpfbo.w = fbo->width();
        mpfbo.h = fbo->height();
        mpfbo.internal_format = 0;
        int flip_y = 0;

        mpv_render_param params[] = {
            // Specify the default framebuffer (0) as target. This will
            // render onto the entire screen. If you want to show the video
            // in a smaller rectangle or apply fancy transformations, you'll
            // need to render into a separate FBO and draw it manually.
            {MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo},
            // Flip rendering (needed due to flipped GL coordinate system).
            {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
            {MPV_RENDER_PARAM_INVALID, nullptr}};
        // See render_gl.h on what OpenGL environment mpv expects, and
        // other API details.
        mpv_render_context_render(m_mpvObject->mpv_gl, params);

        m_mpvObject->window()->resetOpenGLState();
    }

private:
    MpvObject *m_mpvObject = nullptr;
};

MpvObject::MpvObject(QQuickItem *parent)
    : QQuickFramebufferObject(parent),
      mpv(mpv::qt::Handle::FromRawHandle(mpv_create())) {
    Q_ASSERT(mpv != nullptr);

    mpvSetProperty("input-default-bindings", false);
    mpvSetProperty("input-vo-keyboard", false);
    mpvSetProperty("input-cursor", false);
    mpvSetProperty("cursor-autohide", false);

    auto iterator = properties.constBegin();
    while (iterator != properties.constEnd()) {
        mpvObserveProperty(iterator.key());
        ++iterator;
    }

    // From this point on, the wakeup function will be called. The callback
    // can come from any thread, so we use the QueuedConnection mechanism to
    // relay the wakeup in a thread-safe way.
    connect(this, &MpvObject::hasMpvEvents, this, &MpvObject::handleMpvEvents,
            Qt::QueuedConnection);
    mpv_set_wakeup_callback(mpv, wakeup, this);

    const int mpvInitResult = mpv_initialize(mpv);
    Q_ASSERT(mpvInitResult >= 0);

    connect(this, &MpvObject::onUpdate, this, &MpvObject::doUpdate,
            Qt::QueuedConnection);
}

MpvObject::~MpvObject() {
    // only initialized if something got drawn
    if (mpv_gl != nullptr) {
        mpv_render_context_free(mpv_gl);
    }
    // We don't need to destroy mpv handle in our own because we are using
    // mpv::qt::Handle, which is a shared pointer.
    // mpv_terminate_destroy(mpv);
}

void MpvObject::on_update(void *ctx) {
    Q_EMIT static_cast<MpvObject *>(ctx)->onUpdate();
}

// connected to onUpdate() signal makes sure it runs on the GUI thread
void MpvObject::doUpdate() { update(); }

void MpvObject::processMpvLogMessage(mpv_event_log_message *event) {
    switch (event->log_level) {
    case MPV_LOG_LEVEL_V:
    case MPV_LOG_LEVEL_DEBUG:
    case MPV_LOG_LEVEL_TRACE:
        qDebug().noquote() << event->text;
        break;
    case MPV_LOG_LEVEL_WARN:
        qWarning().noquote() << event->text;
        break;
    case MPV_LOG_LEVEL_ERROR:
        qCritical().noquote() << event->text;
        break;
    case MPV_LOG_LEVEL_FATAL:
        // qFatal() doesn't support the "<<" operator.
        qFatal("%s", event->text);
        break;
    case MPV_LOG_LEVEL_INFO:
        qInfo().noquote() << event->text;
        break;
    default:
        qDebug().noquote() << event->text;
        break;
    }
}

void MpvObject::processMpvPropertyChange(mpv_event_property *event) {
    if (!propertyBlackList.contains(event->name)) {
        qDebug().noquote() << "[libmpv] Property changed from mpv:"
                           << event->name;
    }
    if (properties.contains(event->name)) {
        const auto signalName = properties.value(event->name);
        if (signalName != nullptr) {
            QMetaObject::invokeMethod(this, signalName);
        }
    }
}

bool MpvObject::isLoaded() const {
    return ((mediaStatus() == MediaStatus::Loaded) ||
            (mediaStatus() == MediaStatus::Buffering) ||
            (mediaStatus() == MediaStatus::Buffered));
}

bool MpvObject::isPlaying() const {
    return playbackState() == PlaybackState::Playing;
}

bool MpvObject::isPaused() const {
    return playbackState() == PlaybackState::Paused;
}

bool MpvObject::isStopped() const {
    return playbackState() == PlaybackState::Stopped;
}

void MpvObject::setMediaStatus(MpvObject::MediaStatus mediaStatus) {
    if (this->mediaStatus() == mediaStatus) {
        return;
    }
    currentMediaStatus = mediaStatus;
    Q_EMIT mediaStatusChanged();
}

void MpvObject::videoReconfig() { Q_EMIT videoSizeChanged(); }

void MpvObject::audioReconfig() {}

void MpvObject::playbackStateChangeEvent() {
    if (isPlaying()) {
        Q_EMIT playing();
    }
    if (isPaused()) {
        Q_EMIT paused();
    }
    if (isStopped()) {
        Q_EMIT stopped();
    }
    Q_EMIT playbackStateChanged();
}

bool MpvObject::mpvSendCommand(const QVariant &arguments) {
    if (arguments.isNull() || !arguments.isValid()) {
        return false;
    }
    qDebug().noquote() << "Sending a command to mpv:" << arguments;
    int errorCode = 0;
    if (mpvCallType() == MpvCallType::Asynchronous) {
        errorCode = mpv::qt::command_async(mpv, arguments, 0);
    } else {
        errorCode = mpv::qt::get_error(mpv::qt::command(mpv, arguments));
    }
    if (errorCode < 0) {
        qWarning().noquote()
            << "Failed to execute a command for mpv:" << arguments;
    }
    return (errorCode >= 0);
}

bool MpvObject::mpvSetProperty(const char *name, const QVariant &value) {
    if ((name == nullptr) || value.isNull() || !value.isValid()) {
        return false;
    }
    qDebug().noquote() << "Setting a property for mpv:" << name
                       << "to:" << value;
    int errorCode = 0;
    if (mpvCallType() == MpvCallType::Asynchronous) {
        errorCode = mpv::qt::set_property_async(mpv, name, value, 0);
    } else {
        errorCode = mpv::qt::get_error(mpv::qt::set_property(mpv, name, value));
    }
    if (errorCode < 0) {
        qWarning().noquote() << "Failed to set a property for mpv:" << name;
    }
    return (errorCode >= 0);
}

QVariant MpvObject::mpvGetProperty(const char *name, bool *ok) const {
    if (ok != nullptr) {
        *ok = false;
    }
    if (name == nullptr) {
        return QVariant();
    }
    const QVariant result = mpv::qt::get_property(mpv, name);
    if (result.isNull() || !result.isValid()) {
        qWarning().noquote() << "Failed to query a property from mpv:" << name;
    } else {
        if (ok != nullptr) {
            *ok = true;
        }
        /*if ((name != "time-pos") && (name != "duration")) {
            qDebug().noquote() << "Querying a property from mpv:"
                               << name << "result:" << result;
        }*/
    }
    return result;
}

bool MpvObject::mpvObserveProperty(const char *name) {
    if (name == nullptr) {
        return false;
    }
    qDebug().noquote() << "Observing a property from mpv:" << name;
    const int errorCode = mpv_observe_property(mpv, 0, name, MPV_FORMAT_NONE);
    if (errorCode < 0) {
        qWarning().noquote()
            << "Failed to observe a property from mpv:" << name;
    }
    return (errorCode >= 0);
}

QQuickFramebufferObject::Renderer *MpvObject::createRenderer() const {
    window()->setPersistentOpenGLContext(true);
    window()->setPersistentSceneGraph(true);
    return new MpvRenderer(const_cast<MpvObject *>(this));
}

QUrl MpvObject::source() const { return isStopped() ? QUrl() : currentSource; }

QString MpvObject::fileName() const {
    return isStopped() ? QString() : mpvGetProperty("filename").toString();
}

QSize MpvObject::videoSize() const {
    if (isStopped()) {
        return QSize();
    }
    QSize size(qMax(mpvGetProperty("video-out-params/dw").toInt(), 0),
               qMax(mpvGetProperty("video-out-params/dh").toInt(), 0));
    const int rotate = videoRotate();
    if ((rotate == 90) || (rotate == 270)) {
        size.transpose();
    }
    return size;
}

MpvObject::PlaybackState MpvObject::playbackState() const {
    const bool stopped = mpvGetProperty("idle-active").toBool();
    const bool paused = mpvGetProperty("pause").toBool();
    return stopped ? PlaybackState::Stopped
                   : (paused ? PlaybackState::Paused : PlaybackState::Playing);
}

MpvObject::MediaStatus MpvObject::mediaStatus() const {
    return currentMediaStatus;
}

MpvObject::LogLevel MpvObject::logLevel() const {
    const QString level = mpvGetProperty("msg-level").toString();
    if (level.isEmpty() || (level == QString::fromUtf8("no")) ||
        (level == QString::fromUtf8("off"))) {
        return LogLevel::Off;
    }
    const QString actualLevel = level.right(
        level.length() - level.lastIndexOf(QChar::fromLatin1('=')) - 1);
    if (actualLevel.isEmpty() || (actualLevel == QString::fromUtf8("no")) ||
        (actualLevel == QString::fromUtf8("off"))) {
        return LogLevel::Off;
    }
    if ((actualLevel == QString::fromUtf8("v")) ||
        (actualLevel == QString::fromUtf8("debug")) ||
        (actualLevel == QString::fromUtf8("trace"))) {
        return LogLevel::Debug;
    }
    if (actualLevel == QString::fromUtf8("warn")) {
        return LogLevel::Warning;
    }
    if (actualLevel == QString::fromUtf8("error")) {
        return LogLevel::Critical;
    }
    if (actualLevel == QString::fromUtf8("fatal")) {
        return LogLevel::Fatal;
    }
    if (actualLevel == QString::fromUtf8("info")) {
        return LogLevel::Info;
    }
    return LogLevel::Debug;
}

qint64 MpvObject::duration() const {
    return isStopped()
        ? 0
        : qMax(mpvGetProperty("duration").toLongLong(), qint64(0));
}

qint64 MpvObject::position() const {
    return isStopped()
        ? 0
        : qBound(qint64(0), mpvGetProperty("time-pos").toLongLong(),
                 duration());
}

int MpvObject::volume() const {
    return qBound(0, mpvGetProperty("volume").toInt(), 100);
}

bool MpvObject::mute() const { return mpvGetProperty("mute").toBool(); }

bool MpvObject::seekable() const {
    return isStopped() ? false : mpvGetProperty("seekable").toBool();
}

QString MpvObject::mediaTitle() const {
    return isStopped() ? QString() : mpvGetProperty("media-title").toString();
}

QString MpvObject::hwdec() const {
    // Querying "hwdec" itself will return empty string.
    return mpvGetProperty("hwdec-current").toString();
}

QString MpvObject::mpvVersion() const {
    return mpvGetProperty("mpv-version").toString();
}

QString MpvObject::mpvConfiguration() const {
    return mpvGetProperty("mpv-configuration").toString();
}

QString MpvObject::ffmpegVersion() const {
    return mpvGetProperty("ffmpeg-version").toString();
}

int MpvObject::vid() const {
    return isStopped() ? 0 : mpvGetProperty("vid").toInt();
}

int MpvObject::aid() const {
    return isStopped() ? 0 : mpvGetProperty("aid").toInt();
}

int MpvObject::sid() const {
    return isStopped() ? 0 : mpvGetProperty("sid").toInt();
}

int MpvObject::videoRotate() const {
    return isStopped()
        ? 0
        : qMin((qMax(mpvGetProperty("video-out-params/rotate").toInt(), 0) +
                360) %
                   360,
               359);
}

qreal MpvObject::videoAspect() const {
    return isStopped()
        ? 1.7777
        : qMax(mpvGetProperty("video-out-params/aspect").toReal(), 0.0);
}

qreal MpvObject::speed() const {
    return qMax(mpvGetProperty("speed").toReal(), 0.0);
}

bool MpvObject::deinterlace() const {
    return mpvGetProperty("deinterlace").toBool();
}

bool MpvObject::audioExclusive() const {
    return mpvGetProperty("audio-exclusive").toBool();
}

QString MpvObject::audioFileAuto() const {
    return mpvGetProperty("audio-file-auto").toString();
}

QString MpvObject::subAuto() const {
    return mpvGetProperty("sub-auto").toString();
}

QString MpvObject::subCodepage() const {
    QString codePage = mpvGetProperty("sub-codepage").toString();
    if (codePage.startsWith(QChar::fromLatin1('+'))) {
        codePage.remove(0, 1);
    }
    return codePage;
}

QString MpvObject::vo() const { return mpvGetProperty("vo").toString(); }

QString MpvObject::ao() const { return mpvGetProperty("ao").toString(); }

QString MpvObject::screenshotFormat() const {
    return mpvGetProperty("screenshot-format").toString();
}

bool MpvObject::screenshotTagColorspace() const {
    return mpvGetProperty("screenshot-tag-colorspace").toBool();
}

int MpvObject::screenshotPngCompression() const {
    return qBound(0, mpvGetProperty("screenshot-png-compression").toInt(), 9);
}

int MpvObject::screenshotJpegQuality() const {
    return qBound(0, mpvGetProperty("screenshot-jpeg-quality").toInt(), 100);
}

QString MpvObject::screenshotTemplate() const {
    return mpvGetProperty("screenshot-template").toString();
}

QString MpvObject::screenshotDirectory() const {
    return mpvGetProperty("screenshot-directory").toString();
}

QString MpvObject::profile() const {
    return mpvGetProperty("profile").toString();
}

bool MpvObject::hrSeek() const { return mpvGetProperty("hr-seek").toBool(); }

bool MpvObject::ytdl() const { return mpvGetProperty("ytdl").toBool(); }

bool MpvObject::loadScripts() const {
    return mpvGetProperty("load-scripts").toBool();
}

QString MpvObject::path() const {
    return isStopped() ? QString() : mpvGetProperty("path").toString();
}

QString MpvObject::fileFormat() const {
    return isStopped() ? QString() : mpvGetProperty("file-format").toString();
}

qint64 MpvObject::fileSize() const {
    return isStopped()
        ? 0
        : qMax(mpvGetProperty("file-size").toLongLong(), qint64(0));
}

qreal MpvObject::videoBitrate() const {
    return isStopped() ? 0.0
                       : qMax(mpvGetProperty("video-bitrate").toReal(), 0.0);
}

qreal MpvObject::audioBitrate() const {
    return isStopped() ? 0.0
                       : qMax(mpvGetProperty("audio-bitrate").toReal(), 0.0);
}

MpvObject::AudioDevices MpvObject::audioDeviceList() const {
    AudioDevices audioDevices;
    QVariantList deviceList = mpvGetProperty("audio-device-list").toList();
    for (auto &&device : std::as_const(deviceList)) {
        const auto deviceInfo = device.toMap();
        SingleTrackInfo singleTrackInfo;
        singleTrackInfo[QString::fromUtf8("name")] =
            deviceInfo[QString::fromUtf8("name")];
        singleTrackInfo[QString::fromUtf8("description")] =
            deviceInfo[QString::fromUtf8("description")];
        audioDevices.append(singleTrackInfo);
    }
    return audioDevices;
}

QString MpvObject::videoFormat() const {
    return isStopped() ? QString() : mpvGetProperty("video-format").toString();
}

MpvObject::MpvCallType MpvObject::mpvCallType() const {
    return currentMpvCallType;
}

MpvObject::MediaTracks MpvObject::mediaTracks() const {
    MediaTracks mediaTracks;
    QVariantList trackList = mpvGetProperty("track-list").toList();
    for (auto &&track : std::as_const(trackList)) {
        const auto trackInfo = track.toMap();
        if ((trackInfo[QString::fromUtf8("type")] !=
             QString::fromUtf8("video")) &&
            (trackInfo[QString::fromUtf8("type")] !=
             QString::fromUtf8("audio")) &&
            (trackInfo[QString::fromUtf8("type")] !=
             QString::fromUtf8("sub"))) {
            continue;
        }
        SingleTrackInfo singleTrackInfo;
        singleTrackInfo[QString::fromUtf8("id")] =
            trackInfo[QString::fromUtf8("id")];
        singleTrackInfo[QString::fromUtf8("type")] =
            trackInfo[QString::fromUtf8("type")];
        singleTrackInfo[QString::fromUtf8("src-id")] =
            trackInfo[QString::fromUtf8("src-id")];
        if (trackInfo[QString::fromUtf8("title")].toString().isEmpty()) {
            if (trackInfo[QString::fromUtf8("lang")].toString() !=
                QString::fromUtf8("und")) {
                singleTrackInfo[QString::fromUtf8("title")] =
                    trackInfo[QString::fromUtf8("lang")];
            } else if (!trackInfo[QString::fromUtf8("external")].toBool()) {
                singleTrackInfo[QString::fromUtf8("title")] =
                    QString::fromUtf8("[internal]");
            } else {
                singleTrackInfo[QString::fromUtf8("title")] =
                    QString::fromUtf8("[untitled]");
            }
        } else {
            singleTrackInfo[QString::fromUtf8("title")] =
                trackInfo[QString::fromUtf8("title")];
        }
        singleTrackInfo[QString::fromUtf8("lang")] =
            trackInfo[QString::fromUtf8("lang")];
        singleTrackInfo[QString::fromUtf8("default")] =
            trackInfo[QString::fromUtf8("default")];
        singleTrackInfo[QString::fromUtf8("forced")] =
            trackInfo[QString::fromUtf8("forced")];
        singleTrackInfo[QString::fromUtf8("codec")] =
            trackInfo[QString::fromUtf8("codec")];
        singleTrackInfo[QString::fromUtf8("external")] =
            trackInfo[QString::fromUtf8("external")];
        singleTrackInfo[QString::fromUtf8("external-filename")] =
            trackInfo[QString::fromUtf8("external-filename")];
        singleTrackInfo[QString::fromUtf8("selected")] =
            trackInfo[QString::fromUtf8("selected")];
        singleTrackInfo[QString::fromUtf8("decoder-desc")] =
            trackInfo[QString::fromUtf8("decoder-desc")];
        if (trackInfo[QString::fromUtf8("type")] ==
            QString::fromUtf8("video")) {
            singleTrackInfo[QString::fromUtf8("albumart")] =
                trackInfo[QString::fromUtf8("albumart")];
            singleTrackInfo[QString::fromUtf8("demux-w")] =
                trackInfo[QString::fromUtf8("demux-w")];
            singleTrackInfo[QString::fromUtf8("demux-h")] =
                trackInfo[QString::fromUtf8("demux-h")];
            singleTrackInfo[QString::fromUtf8("demux-fps")] =
                trackInfo[QString::fromUtf8("demux-fps")];
            mediaTracks.videoChannels.append(singleTrackInfo);
        } else if (trackInfo[QString::fromUtf8("type")] ==
                   QString::fromUtf8("audio")) {
            singleTrackInfo[QString::fromUtf8("demux-channel-count")] =
                trackInfo[QString::fromUtf8("demux-channel-count")];
            singleTrackInfo[QString::fromUtf8("demux-channels")] =
                trackInfo[QString::fromUtf8("demux-channels")];
            singleTrackInfo[QString::fromUtf8("demux-samplerate")] =
                trackInfo[QString::fromUtf8("demux-samplerate")];
            mediaTracks.audioTracks.append(singleTrackInfo);
        } else if (trackInfo[QString::fromUtf8("type")] ==
                   QString::fromUtf8("sub")) {
            mediaTracks.subtitleStreams.append(singleTrackInfo);
        }
    }
    return mediaTracks;
}

MpvObject::Chapters MpvObject::chapters() const {
    Chapters chapters;
    QVariantList chapterList = mpvGetProperty("chapter-list").toList();
    for (auto &&chapter : std::as_const(chapterList)) {
        const auto chapterInfo = chapter.toMap();
        SingleTrackInfo singleTrackInfo;
        singleTrackInfo[QString::fromUtf8("title")] =
            chapterInfo[QString::fromUtf8("title")];
        singleTrackInfo[QString::fromUtf8("time")] =
            chapterInfo[QString::fromUtf8("time")];
        chapters.append(singleTrackInfo);
    }
    return chapters;
}

MpvObject::Metadata MpvObject::metadata() const {
    Metadata metadata;
    QVariantMap metadataMap = mpvGetProperty("metadata").toMap();
    auto iterator = metadataMap.constBegin();
    while (iterator != metadataMap.constEnd()) {
        metadata[iterator.key()] = iterator.value();
        ++iterator;
    }
    return metadata;
}

qreal MpvObject::avsync() const {
    return isStopped() ? 0.0 : qMax(mpvGetProperty("avsync").toReal(), 0.0);
}

int MpvObject::percentPos() const {
    return isStopped() ? 0
                       : qBound(0, mpvGetProperty("percent-pos").toInt(), 100);
}

qreal MpvObject::estimatedVfFps() const {
    return isStopped() ? 0.0
                       : qMax(mpvGetProperty("estimated-vf-fps").toReal(), 0.0);
}

bool MpvObject::open(const QUrl &url) {
    if (!url.isValid()) {
        return false;
    }
    if (url != currentSource) {
        setSource(url);
    }
    if (!isPlaying()) {
        play();
    }
    return true;
}

bool MpvObject::play() {
    if (!isPaused() || !currentSource.isValid()) {
        return false;
    }
    const bool result = mpvSetProperty("pause", false);
    if (result) {
        Q_EMIT playing();
    }
    return result;
}

bool MpvObject::play(const QUrl &url) {
    if (!url.isValid()) {
        return false;
    }
    bool result = false;
    if ((url == currentSource) && !isPlaying()) {
        result = play();
    } else {
        result = open(url);
    }
    return result;
}

bool MpvObject::pause() {
    if (!isPlaying()) {
        return false;
    }
    const bool result = mpvSetProperty("pause", true);
    if (result) {
        Q_EMIT paused();
    }
    return result;
}

bool MpvObject::stop() {
    if (isStopped()) {
        return false;
    }
    const bool result = mpvSendCommand(QVariantList{QString::fromUtf8("stop")});
    if (result) {
        Q_EMIT stopped();
    }
    currentSource.clear();
    return result;
}

bool MpvObject::seek(qint64 value, bool absolute, bool percent) {
    if (isStopped()) {
        return false;
    }
    const qint64 min = (absolute || percent) ? 0 : -position();
    const qint64 max =
        percent ? 100 : (absolute ? duration() : duration() - position());
    return mpvSendCommand(
        QVariantList{QString::fromUtf8("seek"), qBound(min, value, max),
                     percent ? QString::fromUtf8("absolute-percent")
                             : (absolute ? QString::fromUtf8("absolute")
                                         : QString::fromUtf8("relative"))});
}

bool MpvObject::seekAbsolute(qint64 position) {
    if (isStopped() || (position == this->position())) {
        return false;
    }
    return seek(qBound(qint64(0), position, duration()), true);
}

bool MpvObject::seekRelative(qint64 offset) {
    if (isStopped() || (offset == 0)) {
        return false;
    }
    return seek(qBound(-position(), offset, duration() - position()));
}

bool MpvObject::seekPercent(int percent) {
    if (isStopped() || (percent == this->percentPos())) {
        return false;
    }
    return seek(qBound(0, percent, 100), true, true);
}

bool MpvObject::screenshot() {
    if (isStopped()) {
        return false;
    }
    // Replace "subtitles" with "video" if you don't want to include subtitles
    // when screenshotting.
    return mpvSendCommand(QVariantList{QString::fromUtf8("screenshot"),
                                       QString::fromUtf8("subtitles")});
}

bool MpvObject::screenshotToFile(const QString &filePath) {
    if (isStopped() || filePath.isEmpty()) {
        return false;
    }
    // libmpv's default: including subtitles when making a screenshot.
    return mpvSendCommand(QVariantList{QString::fromUtf8("screenshot-to-file"),
                                       filePath,
                                       QString::fromUtf8("subtitles")});
}

void MpvObject::setSource(const QUrl &source) {
    if (!source.isValid() || (source == currentSource)) {
        return;
    }
    const bool result = mpvSendCommand(QVariantList{
        QString::fromUtf8("loadfile"),
        source.isLocalFile() ? source.toLocalFile() : source.url()});
    if (result) {
        currentSource = source;
        Q_EMIT sourceChanged();
    }
}

void MpvObject::setMute(bool mute) {
    if (mute == this->mute()) {
        return;
    }
    mpvSetProperty("mute", mute);
}

void MpvObject::setPlaybackState(MpvObject::PlaybackState playbackState) {
    if (isStopped() || (this->playbackState() == playbackState)) {
        return;
    }
    bool result = false;
    switch (playbackState) {
    case PlaybackState::Stopped:
        result = stop();
        break;
    case PlaybackState::Paused:
        result = pause();
        break;
    case PlaybackState::Playing:
        result = play();
        break;
    }
    if (result) {
        Q_EMIT playbackStateChanged();
    }
}

void MpvObject::setLogLevel(MpvObject::LogLevel logLevel) {
    if (logLevel == this->logLevel()) {
        return;
    }
    QString level = QString::fromUtf8("debug");
    switch (logLevel) {
    case LogLevel::Off:
        level = QString::fromUtf8("no");
        break;
    case LogLevel::Debug:
        // libmpv's log level: v (verbose) < debug < trace (print all messages)
        // Use "v" to avoid noisy message floods.
        level = QString::fromUtf8("v");
        break;
    case LogLevel::Warning:
        level = QString::fromUtf8("warn");
        break;
    case LogLevel::Critical:
        level = QString::fromUtf8("error");
        break;
    case LogLevel::Fatal:
        level = QString::fromUtf8("fatal");
        break;
    case LogLevel::Info:
        level = QString::fromUtf8("info");
        break;
    }
    const bool result1 =
        mpvSetProperty("terminal", level != QString::fromUtf8("no"));
    const bool result2 =
        mpvSetProperty("msg-level", QString::fromUtf8("all=%1").arg(level));
    const int result3 =
        mpv_request_log_messages(mpv, level.toUtf8().constData());
    if (result1 && result2 && (result3 >= 0)) {
        Q_EMIT logLevelChanged();
    } else {
        qWarning().noquote() << "Failed to set log level.";
    }
}

void MpvObject::setPosition(qint64 position) {
    if (isStopped() || (position == this->position())) {
        return;
    }
    seek(qBound(qint64(0), position, duration()));
}

void MpvObject::setVolume(int volume) {
    if (volume == this->volume()) {
        return;
    }
    mpvSetProperty("volume", qBound(0, volume, 100));
}

void MpvObject::setHwdec(const QString &hwdec) {
    if (hwdec.isEmpty() || (hwdec == this->hwdec())) {
        return;
    }
    mpvSetProperty("hwdec", hwdec);
}

void MpvObject::setVid(int vid) {
    if (isStopped() || (vid == this->vid())) {
        return;
    }
    mpvSetProperty("vid", qMax(vid, 0));
}

void MpvObject::setAid(int aid) {
    if (isStopped() || (aid == this->aid())) {
        return;
    }
    mpvSetProperty("aid", qMax(aid, 0));
}

void MpvObject::setSid(int sid) {
    if (isStopped() || (sid == this->sid())) {
        return;
    }
    mpvSetProperty("sid", qMax(sid, 0));
}

void MpvObject::setVideoRotate(int videoRotate) {
    if (isStopped() || (videoRotate == this->videoRotate())) {
        return;
    }
    mpvSetProperty("video-rotate", qBound(0, videoRotate, 359));
}

void MpvObject::setVideoAspect(qreal videoAspect) {
    if (isStopped() || (videoAspect == this->videoAspect())) {
        return;
    }
    mpvSetProperty("video-aspect", qMax(videoAspect, 0.0));
}

void MpvObject::setSpeed(qreal speed) {
    if (isStopped() || (speed == this->speed())) {
        return;
    }
    mpvSetProperty("speed", qMax(speed, 0.0));
}

void MpvObject::setDeinterlace(bool deinterlace) {
    if (deinterlace == this->deinterlace()) {
        return;
    }
    mpvSetProperty("deinterlace", deinterlace);
}

void MpvObject::setAudioExclusive(bool audioExclusive) {
    if (audioExclusive == this->audioExclusive()) {
        return;
    }
    mpvSetProperty("audio-exclusive", audioExclusive);
}

void MpvObject::setAudioFileAuto(const QString &audioFileAuto) {
    if (audioFileAuto.isEmpty() || (audioFileAuto == this->audioFileAuto())) {
        return;
    }
    mpvSetProperty("audio-file-auto", audioFileAuto);
}

void MpvObject::setSubAuto(const QString &subAuto) {
    if (subAuto.isEmpty() || (subAuto == this->subAuto())) {
        return;
    }
    mpvSetProperty("sub-auto", subAuto);
}

void MpvObject::setSubCodepage(const QString &subCodepage) {
    if (subCodepage.isEmpty() || (subCodepage == this->subCodepage())) {
        return;
    }
    mpvSetProperty("sub-codepage",
                   subCodepage.startsWith(QChar::fromLatin1('+'))
                       ? subCodepage
                       : (subCodepage.startsWith(QString::fromUtf8("cp"))
                              ? QChar::fromLatin1('+') + subCodepage
                              : subCodepage));
}

void MpvObject::setVo(const QString &vo) {
    if (vo.isEmpty() || (vo == this->vo())) {
        return;
    }
    mpvSetProperty("vo", vo);
}

void MpvObject::setAo(const QString &ao) {
    if (ao.isEmpty() || (ao == this->ao())) {
        return;
    }
    mpvSetProperty("ao", ao);
}

void MpvObject::setScreenshotFormat(const QString &screenshotFormat) {
    if (screenshotFormat.isEmpty() ||
        (screenshotFormat == this->screenshotFormat())) {
        return;
    }
    mpvSetProperty("screenshot-format", screenshotFormat);
}

void MpvObject::setScreenshotPngCompression(int screenshotPngCompression) {
    if (screenshotPngCompression == this->screenshotPngCompression()) {
        return;
    }
    mpvSetProperty("screenshot-png-compression",
                   qBound(0, screenshotPngCompression, 9));
}

void MpvObject::setScreenshotTemplate(const QString &screenshotTemplate) {
    if (screenshotTemplate.isEmpty() ||
        (screenshotTemplate == this->screenshotTemplate())) {
        return;
    }
    mpvSetProperty("screenshot-template", screenshotTemplate);
}

void MpvObject::setScreenshotDirectory(const QString &screenshotDirectory) {
    if (screenshotDirectory.isEmpty() ||
        (screenshotDirectory == this->screenshotDirectory())) {
        return;
    }
    mpvSetProperty("screenshot-directory", screenshotDirectory);
}

void MpvObject::setProfile(const QString &profile) {
    if (profile.isEmpty() || (profile == this->profile())) {
        return;
    }
    mpvSendCommand(QVariantList{QString::fromUtf8("apply-profile"), profile});
}

void MpvObject::setHrSeek(bool hrSeek) {
    if (hrSeek == this->hrSeek()) {
        return;
    }
    mpvSetProperty("hr-seek",
                   hrSeek ? QString::fromUtf8("yes") : QString::fromUtf8("no"));
}

void MpvObject::setYtdl(bool ytdl) {
    if (ytdl == this->ytdl()) {
        return;
    }
    mpvSetProperty("ytdl", ytdl);
}

void MpvObject::setLoadScripts(bool loadScripts) {
    if (loadScripts == this->loadScripts()) {
        return;
    }
    mpvSetProperty("load-scripts", loadScripts);
}

void MpvObject::setScreenshotTagColorspace(bool screenshotTagColorspace) {
    if (screenshotTagColorspace == this->screenshotTagColorspace()) {
        return;
    }
    mpvSetProperty("screenshot-tag-colorspace", screenshotTagColorspace);
}

void MpvObject::setScreenshotJpegQuality(int screenshotJpegQuality) {
    if (screenshotJpegQuality == this->screenshotJpegQuality()) {
        return;
    }
    mpvSetProperty("screenshot-jpeg-quality",
                   qBound(0, screenshotJpegQuality, 100));
}

void MpvObject::setMpvCallType(MpvObject::MpvCallType mpvCallType) {
    if (this->mpvCallType() == mpvCallType) {
        return;
    }
    currentMpvCallType = mpvCallType;
    Q_EMIT mpvCallTypeChanged();
}

void MpvObject::setPercentPos(int percentPos) {
    if (isStopped() || (percentPos == this->percentPos())) {
        return;
    }
    mpvSetProperty("percent-pos", qBound(0, percentPos, 100));
}

void MpvObject::handleMpvEvents() {
    // Process all events, until the event queue is empty.
    while (mpv != nullptr) {
        mpv_event *event = mpv_wait_event(mpv, 0.005);
        // Nothing happened. Happens on timeouts or sporadic wakeups.
        if (event->event_id == MPV_EVENT_NONE) {
            break;
        }
        bool shouldOutput = true;
        switch (event->event_id) {
        // Happens when the player quits. The player enters a state where it
        // tries to disconnect all clients. Most requests to the player will
        // fail, and the client should react to this and quit with
        // mpv_destroy() as soon as possible.
        case MPV_EVENT_SHUTDOWN:
            break;
        // See mpv_request_log_messages().
        case MPV_EVENT_LOG_MESSAGE:
            processMpvLogMessage(
                static_cast<mpv_event_log_message *>(event->data));
            shouldOutput = false;
            break;
        // Reply to a mpv_get_property_async() request.
        // See also mpv_event and mpv_event_property.
        case MPV_EVENT_GET_PROPERTY_REPLY:
            shouldOutput = false;
            break;
        // Reply to a mpv_set_property_async() request.
        // (Unlike MPV_EVENT_GET_PROPERTY, mpv_event_property is not used.)
        case MPV_EVENT_SET_PROPERTY_REPLY:
            shouldOutput = false;
            break;
        // Reply to a mpv_command_async() or mpv_command_node_async() request.
        // See also mpv_event and mpv_event_command.
        case MPV_EVENT_COMMAND_REPLY:
            shouldOutput = false;
            break;
        // Notification before playback start of a file (before the file is
        // loaded).
        case MPV_EVENT_START_FILE:
            setMediaStatus(MediaStatus::Loading);
            break;
        // Notification after playback end (after the file was unloaded).
        // See also mpv_event and mpv_event_end_file.
        case MPV_EVENT_END_FILE:
            setMediaStatus(MediaStatus::End);
            playbackStateChangeEvent();
            break;
        // Notification when the file has been loaded (headers were read
        // etc.), and decoding starts.
        case MPV_EVENT_FILE_LOADED:
            setMediaStatus(MediaStatus::Loaded);
            Q_EMIT loaded();
            playbackStateChangeEvent();
            break;
        // Idle mode was entered. In this mode, no file is played, and the
        // playback core waits for new commands. (The command line player
        // normally quits instead of entering idle mode, unless --idle was
        // specified. If mpv was started with mpv_create(), idle mode is enabled
        // by default.)
        case MPV_EVENT_IDLE:
            playbackStateChangeEvent();
            break;
        // Triggered by the script-message input command. The command uses the
        // first argument of the command as client name (see mpv_client_name())
        // to dispatch the message, and passes along all arguments starting from
        // the second argument as strings.
        // See also mpv_event and mpv_event_client_message.
        case MPV_EVENT_CLIENT_MESSAGE:
            break;
        // Happens after video changed in some way. This can happen on
        // resolution changes, pixel format changes, or video filter changes.
        // The event is sent after the video filters and the VO are
        // reconfigured. Applications embedding a mpv window should listen to
        // this event in order to resize the window if needed.
        // Note that this event can happen sporadically, and you should check
        // yourself whether the video parameters really changed before doing
        // something expensive.
        case MPV_EVENT_VIDEO_RECONFIG:
            videoReconfig();
            break;
        // Similar to MPV_EVENT_VIDEO_RECONFIG. This is relatively
        // uninteresting, because there is no such thing as audio output
        // embedding.
        case MPV_EVENT_AUDIO_RECONFIG:
            audioReconfig();
            break;
        // Happens when a seek was initiated. Playback stops. Usually it will
        // resume with MPV_EVENT_PLAYBACK_RESTART as soon as the seek is
        // finished.
        case MPV_EVENT_SEEK:
            break;
        // There was a discontinuity of some sort (like a seek), and playback
        // was reinitialized. Usually happens after seeking, or ordered chapter
        // segment switches. The main purpose is allowing the client to detect
        // when a seek request is finished.
        case MPV_EVENT_PLAYBACK_RESTART:
            break;
        // Event sent due to mpv_observe_property().
        // See also mpv_event and mpv_event_property.
        case MPV_EVENT_PROPERTY_CHANGE:
            processMpvPropertyChange(
                static_cast<mpv_event_property *>(event->data));
            shouldOutput = false;
            break;
        // Happens if the internal per-mpv_handle ringbuffer overflows, and at
        // least 1 event had to be dropped. This can happen if the client
        // doesn't read the event queue quickly enough with mpv_wait_event(), or
        // if the client makes a very large number of asynchronous calls at
        // once.
        // Event delivery will continue normally once this event was returned
        // (this forces the client to empty the queue completely).
        case MPV_EVENT_QUEUE_OVERFLOW:
            break;
        // Triggered if a hook handler was registered with mpv_hook_add(), and
        // the hook is invoked. If you receive this, you must handle it, and
        // continue the hook with mpv_hook_continue().
        // See also mpv_event and mpv_event_hook.
        case MPV_EVENT_HOOK:
            break;
        default:
            break;
        }
        if (shouldOutput) {
            qDebug().noquote()
                << "[libmpv] Event received from mpv:"
                << QString::fromUtf8(mpv_event_name(event->event_id));
        }
    }
}
