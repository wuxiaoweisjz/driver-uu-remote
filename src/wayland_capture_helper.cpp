#include <QByteArray>
#include <QColor>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QTimer>

#include <atomic>
#include <QVariantMap>
#include <QWaitCondition>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <libei.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "capture_protocol.h"

struct FrameState {
    QMutex mutex;
    QWaitCondition ready;
    QByteArray pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t generation = 0;
};

static FrameState frame_state;
static QMutex kwin_capture_mutex;
static QByteArray required_auth_token;
static bool kwin_capture_on_request;
static std::atomic_uint active_client_count{0};

static const char portal_service[] = "org.freedesktop.portal.Desktop";
static const char portal_path[] = "/org/freedesktop/portal/desktop";
static const char screen_cast_interface[] = "org.freedesktop.portal.ScreenCast";
static const char request_interface[] = "org.freedesktop.portal.Request";

class ScreenSaverInhibitor {
public:
    bool start()
    {
        QDBusInterface screen_saver(QStringLiteral("org.freedesktop.ScreenSaver"),
                                    QStringLiteral("/ScreenSaver"),
                                    QStringLiteral("org.freedesktop.ScreenSaver"),
                                    QDBusConnection::sessionBus());
        if (!screen_saver.isValid()) {
            fprintf(stderr, "uu-wayland-capture-helper: screen saver service unavailable\n");
            return false;
        }
        QDBusReply<bool> active = screen_saver.call(QStringLiteral("GetActive"));
        if (!active.isValid() || active.value()) {
            fprintf(stderr, "uu-wayland-capture-helper: desktop is locked; waiting for unlock\n");
            return false;
        }
        QDBusReply<uint> reply = screen_saver.call(
            QStringLiteral("Inhibit"), QStringLiteral("UU Remote"),
            QStringLiteral("Keep the shared Wayland desktop available"));
        if (!reply.isValid()) {
            fprintf(stderr, "uu-wayland-capture-helper: failed to inhibit screen locking\n");
            return false;
        }
        cookie_ = reply.value();
        fprintf(stderr, "uu-wayland-capture-helper: automatic screen locking inhibited\n");
        return true;
    }

    ~ScreenSaverInhibitor()
    {
        if (!cookie_) return;
        QDBusInterface screen_saver(QStringLiteral("org.freedesktop.ScreenSaver"),
                                    QStringLiteral("/ScreenSaver"),
                                    QStringLiteral("org.freedesktop.ScreenSaver"),
                                    QDBusConnection::sessionBus());
        screen_saver.call(QDBus::NoBlock, QStringLiteral("UnInhibit"), cookie_);
    }

private:
    uint cookie_ = 0;
};

static void publish_image(const QImage &source)
{
    QImage image = source.convertToFormat(QImage::Format_ARGB32);
    QByteArray pixels;
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) return;
    pixels.resize(image.width() * image.height() * 4);
    for (int y = 0; y < image.height(); ++y) {
        memcpy(pixels.data() + static_cast<size_t>(y) * image.width() * 4,
               image.constScanLine(y), static_cast<size_t>(image.width()) * 4);
    }
    QMutexLocker locker(&frame_state.mutex);
    frame_state.pixels = std::move(pixels);
    frame_state.width = static_cast<uint32_t>(image.width());
    frame_state.height = static_cast<uint32_t>(image.height());
    ++frame_state.generation;
    frame_state.ready.wakeAll();
}

static void publish_bgra(const uint8_t *source, uint32_t width, uint32_t height,
                         int stride)
{
    QByteArray pixels;
    const size_t row_size = static_cast<size_t>(width) * 4;
    if (!source || !width || !height || stride < static_cast<int>(row_size)) return;
    pixels.resize(static_cast<qsizetype>(row_size * height));
    for (uint32_t y = 0; y < height; ++y) {
        memcpy(pixels.data() + static_cast<size_t>(y) * row_size,
               source + static_cast<size_t>(y) * stride, row_size);
    }
    QMutexLocker locker(&frame_state.mutex);
    frame_state.pixels = std::move(pixels);
    frame_state.width = width;
    frame_state.height = height;
    ++frame_state.generation;
    frame_state.ready.wakeAll();
}

static bool read_exact_fd(int fd, void *data, size_t size)
{
    uint8_t *cursor = static_cast<uint8_t *>(data);
    while (size) {
        ssize_t count = read(fd, cursor, size);
        if (!count) return false;
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        cursor += count;
        size -= static_cast<size_t>(count);
    }
    return true;
}

static bool capture_kwin_workspace()
{
    constexpr uint64_t max_frame_size = 256u * 1024u * 1024u;
    int pipe_fds[2] = {-1, -1};
    QMutexLocker capture_locker(&kwin_capture_mutex);
    if (pipe(pipe_fds) < 0) return false;
    QDBusMessage request = QDBusMessage::createMethodCall(
        QStringLiteral("org.kde.KWin"), QStringLiteral("/org/kde/KWin/ScreenShot2"),
        QStringLiteral("org.kde.KWin.ScreenShot2"), QStringLiteral("CaptureWorkspace"));
    QVariantMap options;
    options.insert(QStringLiteral("include-cursor"), true);
    options.insert(QStringLiteral("native-resolution"), false);
    options.insert(QStringLiteral("hide-caller-windows"), false);
    QDBusUnixFileDescriptor descriptor(pipe_fds[1]);
    request << options << QVariant::fromValue(descriptor);
    QDBusMessage response = QDBusConnection::sessionBus().call(
        request, QDBus::Block, 3000);
    close(pipe_fds[1]);
    pipe_fds[1] = -1;
    if (response.type() == QDBusMessage::ErrorMessage || response.arguments().isEmpty()) {
        fprintf(stderr, "uu-wayland-capture-helper: KWin screenshot failed: %s\n",
                response.errorMessage().toUtf8().constData());
        close(pipe_fds[0]);
        return false;
    }
    const QVariantMap results = qdbus_cast<QVariantMap>(response.arguments().first());
    const uint32_t width = results.value(QStringLiteral("width")).toUInt();
    const uint32_t height = results.value(QStringLiteral("height")).toUInt();
    const uint32_t stride = results.value(QStringLiteral("stride")).toUInt();
    const uint32_t format = results.value(QStringLiteral("format")).toUInt();
    const uint64_t raw_size = static_cast<uint64_t>(stride) * height;
    const uint64_t output_size = static_cast<uint64_t>(width) * height * 4u;
    if (!width || !height || stride < width * 4u || raw_size > max_frame_size ||
        output_size > max_frame_size) {
        close(pipe_fds[0]);
        return false;
    }
    QByteArray raw(static_cast<qsizetype>(raw_size), Qt::Uninitialized);
    QByteArray pixels(static_cast<qsizetype>(output_size), Qt::Uninitialized);
    if (!read_exact_fd(pipe_fds[0], raw.data(), static_cast<size_t>(raw_size))) {
        close(pipe_fds[0]);
        return false;
    }
    close(pipe_fds[0]);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *source = reinterpret_cast<const uint8_t *>(raw.constData()) +
            static_cast<size_t>(y) * stride;
        uint8_t *destination = reinterpret_cast<uint8_t *>(pixels.data()) +
            static_cast<size_t>(y) * width * 4u;
        memcpy(destination, source, static_cast<size_t>(width) * 4u);
        if (format == 16u || format == 17u || format == 18u) {
            for (uint32_t x = 0; x < width; ++x) {
                uint8_t red = destination[x * 4u];
                destination[x * 4u] = destination[x * 4u + 2u];
                destination[x * 4u + 2u] = red;
            }
        } else if (format != 4u && format != 5u && format != 6u) {
            fprintf(stderr, "uu-wayland-capture-helper: unsupported KWin format %u\n",
                    format);
            return false;
        }
    }
    {
        QMutexLocker frame_locker(&frame_state.mutex);
        frame_state.pixels = std::move(pixels);
        frame_state.width = width;
        frame_state.height = height;
        ++frame_state.generation;
        frame_state.ready.wakeAll();
    }
    return true;
}

class PortalResponse : public QObject {
    Q_OBJECT

public:
    explicit PortalResponse(const QString &path) : path_(path)
    {
        connected_ = QDBusConnection::sessionBus().connect(
            portal_service, path_, request_interface, "Response", this,
            SLOT(response(uint,QVariantMap)));
    }

    ~PortalResponse() override
    {
        if (connected_) {
            QDBusConnection::sessionBus().disconnect(
                portal_service, path_, request_interface, "Response", this,
                SLOT(response(uint,QVariantMap)));
        }
    }

    bool wait(uint *response_code, QVariantMap *results, QString *error)
    {
        QTimer timer;
        if (!connected_) {
            *error = QStringLiteral("failed to subscribe to Portal response");
            return false;
        }
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop_, &QEventLoop::quit);
        timer.start(300000);
        if (!received_) loop_.exec();
        if (!received_) {
            *error = QStringLiteral("Portal request timed out");
            return false;
        }
        *response_code = response_code_;
        *results = results_;
        return true;
    }

private slots:
    void response(uint response_code, const QVariantMap &results)
    {
        response_code_ = response_code;
        results_ = results;
        received_ = true;
        loop_.quit();
    }

private:
    QString path_;
    QEventLoop loop_;
    QVariantMap results_;
    uint response_code_ = 2;
    bool connected_ = false;
    bool received_ = false;
};

static QString request_path_for_token(const QString &token)
{
    QString sender = QDBusConnection::sessionBus().baseService();
    if (sender.startsWith(QLatin1Char(':'))) sender.remove(0, 1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
        .arg(sender, token);
}

static QString new_portal_token(const char *kind)
{
    return QStringLiteral("uu_%1_%2_%3")
        .arg(QString::fromLatin1(kind))
        .arg(static_cast<qulonglong>(getpid()))
        .arg(QRandomGenerator::global()->generate64(), 0, 16);
}

static QString variant_string(const QVariant &value)
{
    if (value.canConvert<QDBusVariant>())
        return variant_string(value.value<QDBusVariant>().variant());
    if (value.canConvert<QDBusObjectPath>())
        return value.value<QDBusObjectPath>().path();
    return value.toString();
}

static bool first_stream_node(const QVariant &value, uint32_t *node_id)
{
    QVariant unwrapped = value;
    if (unwrapped.canConvert<QDBusVariant>())
        unwrapped = unwrapped.value<QDBusVariant>().variant();
    if (!unwrapped.canConvert<QDBusArgument>()) return false;

    const QDBusArgument argument = unwrapped.value<QDBusArgument>();
    argument.beginArray();
    if (argument.atEnd()) {
        argument.endArray();
        return false;
    }
    QVariantMap properties;
    argument.beginStructure();
    argument >> *node_id >> properties;
    argument.endStructure();
    argument.endArray();
    return *node_id != 0;
}

static QString restore_token_path()
{
    const QString override_path = qEnvironmentVariable("UU_PORTAL_RESTORE_TOKEN_FILE");
    if (!override_path.isEmpty()) return override_path;
    QString state_home = qEnvironmentVariable("XDG_STATE_HOME");
    if (state_home.isEmpty()) state_home = QDir::homePath() + QStringLiteral("/.local/state");
    return state_home + QStringLiteral("/uu-amf-bridge/portal-restore-token");
}

static QString load_restore_token(const QString &path)
{
    QFileInfo info(path);
    if (!info.exists()) return {};
    if (info.isSymLink() || !info.isFile()) {
        fprintf(stderr, "uu-wayland-capture-helper: refusing unsafe restore token path\n");
        return {};
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray value = file.read(8193).trimmed();
    if (value.isEmpty() || value.size() > 8192) return {};
    return QString::fromUtf8(value);
}

static bool save_restore_token(const QString &path, const QString &token)
{
    QFileInfo info(path);
    QDir directory(info.absolutePath());
    if (!directory.mkpath(QStringLiteral("."))) return false;
    chmod(QFile::encodeName(directory.absolutePath()).constData(), 0700);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QByteArray value = token.toUtf8();
    value.append('\n');
    if (file.write(value) != value.size() || !file.commit()) return false;
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

class PortalCapture {
public:
    bool start(int *pipewire_fd, uint32_t *node_id)
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected()) {
            fprintf(stderr, "uu-wayland-capture-helper: session D-Bus is unavailable\n");
            return false;
        }
        QDBusInterface portal(portal_service, portal_path, screen_cast_interface, bus);
        if (!portal.isValid()) {
            fprintf(stderr, "uu-wayland-capture-helper: ScreenCast Portal is unavailable\n");
            return false;
        }

        QVariantMap create_options;
        create_options.insert(QStringLiteral("session_handle_token"),
                              new_portal_token("session"));
        QVariantMap create_results;
        if (!request(portal, QStringLiteral("CreateSession"), {}, &create_options,
                     &create_results)) return false;
        session_path_ = variant_string(create_results.value(QStringLiteral("session_handle")));
        if (session_path_.isEmpty()) {
            fprintf(stderr, "uu-wayland-capture-helper: Portal returned no session handle\n");
            return false;
        }

        token_path_ = restore_token_path();
        const QString restore_token = load_restore_token(token_path_);
        QVariantMap select_options;
        select_options.insert(QStringLiteral("types"), 1u);
        select_options.insert(QStringLiteral("multiple"), false);
        select_options.insert(QStringLiteral("cursor_mode"), 2u);
        select_options.insert(QStringLiteral("persist_mode"), 2u);
        if (!restore_token.isEmpty())
            select_options.insert(QStringLiteral("restore_token"), restore_token);
        if (!request(portal, QStringLiteral("SelectSources"),
                     {QVariant::fromValue(QDBusObjectPath(session_path_))},
                     &select_options, nullptr)) return false;

        QVariantMap start_options;
        QVariantMap start_results;
        if (!request(portal, QStringLiteral("Start"),
                     {QVariant::fromValue(QDBusObjectPath(session_path_)), QString()},
                     &start_options, &start_results)) return false;
        if (!first_stream_node(start_results.value(QStringLiteral("streams")), node_id)) {
            fprintf(stderr, "uu-wayland-capture-helper: Portal returned no PipeWire stream\n");
            return false;
        }

        const QString next_token = variant_string(
            start_results.value(QStringLiteral("restore_token")));
        if (next_token.isEmpty() || !save_restore_token(token_path_, next_token)) {
            fprintf(stderr, "uu-wayland-capture-helper: Portal did not persist a restore token\n");
            return false;
        }
        fprintf(stderr, "uu-wayland-capture-helper: Portal permission %s; restore token rotated\n",
                restore_token.isEmpty() ? "created" : "restored");

        QDBusMessage reply = portal.call(
            QStringLiteral("OpenPipeWireRemote"),
            QVariant::fromValue(QDBusObjectPath(session_path_)), QVariantMap());
        if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
            fprintf(stderr, "uu-wayland-capture-helper: OpenPipeWireRemote failed: %s\n",
                    reply.errorMessage().toUtf8().constData());
            return false;
        }
        const QDBusUnixFileDescriptor descriptor =
            reply.arguments().first().value<QDBusUnixFileDescriptor>();
        *pipewire_fd = dup(descriptor.fileDescriptor());
        if (*pipewire_fd < 0) {
            perror("dup PipeWire fd");
            return false;
        }
        return true;
    }

    ~PortalCapture()
    {
        if (session_path_.isEmpty()) return;
        QDBusInterface session(portal_service, session_path_,
                               QStringLiteral("org.freedesktop.portal.Session"),
                               QDBusConnection::sessionBus());
        session.call(QDBus::NoBlock, QStringLiteral("Close"));
    }

private:
    bool request(QDBusInterface &portal, const QString &method,
                 const QVariantList &arguments, QVariantMap *options,
                 QVariantMap *results)
    {
        const QString token = new_portal_token("request");
        options->insert(QStringLiteral("handle_token"), token);
        PortalResponse response(request_path_for_token(token));
        QVariantList call_arguments = arguments;
        call_arguments.append(*options);
        QDBusMessage reply = portal.callWithArgumentList(QDBus::Block, method, call_arguments);
        if (reply.type() == QDBusMessage::ErrorMessage) {
            fprintf(stderr, "uu-wayland-capture-helper: Portal %s failed: %s\n",
                    method.toUtf8().constData(), reply.errorMessage().toUtf8().constData());
            return false;
        }
        uint response_code;
        QVariantMap response_results;
        QString error;
        if (!response.wait(&response_code, &response_results, &error)) {
            fprintf(stderr, "uu-wayland-capture-helper: Portal %s failed: %s\n",
                    method.toUtf8().constData(), error.toUtf8().constData());
            return false;
        }
        if (response_code != 0) {
            fprintf(stderr, "uu-wayland-capture-helper: Portal %s rejected (response %u)\n",
                    method.toUtf8().constData(), response_code);
            return false;
        }
        if (results) *results = response_results;
        return true;
    }

    QString session_path_;
    QString token_path_;
};

class GStreamerCapture {
public:
    bool start(int pipewire_fd, uint32_t node_id)
    {
        pipeline_ = gst_pipeline_new("uu-wayland-capture");
        GstElement *source = gst_element_factory_make("pipewiresrc", "source");
        GstElement *convert = gst_element_factory_make("videoconvert", "convert");
        sink_ = gst_element_factory_make("appsink", "sink");
        if (!pipeline_ || !source || !convert || !sink_) {
            fprintf(stderr, "uu-wayland-capture-helper: missing GStreamer capture elements\n");
            close(pipewire_fd);
            return false;
        }

        QByteArray node = QByteArray::number(node_id);
        g_object_set(source, "fd", pipewire_fd, "path", node.constData(),
                     "client-name", "UU Wayland Capture", "do-timestamp", TRUE, nullptr);
        GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING,
                                            "BGRA", nullptr);
        g_object_set(sink_, "caps", caps, "emit-signals", FALSE, "max-buffers", 2u,
                     "drop", TRUE, "sync", FALSE, nullptr);
        gst_caps_unref(caps);

        GstAppSinkCallbacks callbacks = {};
        callbacks.new_sample = new_sample;
        gst_app_sink_set_callbacks(GST_APP_SINK(sink_), &callbacks, this, nullptr);
        gst_bin_add_many(GST_BIN(pipeline_), source, convert, sink_, nullptr);
        if (!gst_element_link_many(source, convert, sink_, nullptr)) {
            fprintf(stderr, "uu-wayland-capture-helper: failed to link GStreamer capture\n");
            return false;
        }
        GstBus *bus = gst_element_get_bus(pipeline_);
        gst_bus_set_sync_handler(bus, bus_message, nullptr, nullptr);
        gst_object_unref(bus);
        if (gst_element_set_state(pipeline_, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
            fprintf(stderr, "uu-wayland-capture-helper: failed to prepare GStreamer capture\n");
            return false;
        }
        return true;
    }

    bool resume()
    {
        QMutexLocker locker(&state_mutex_);
        if (!pipeline_) return true;
        if (playing_) return true;
        if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            fprintf(stderr, "uu-wayland-capture-helper: failed to resume GStreamer capture\n");
            return false;
        }
        playing_ = true;
        return true;
    }

    void suspend()
    {
        QMutexLocker locker(&state_mutex_);
        if (!pipeline_) return;
        if (!playing_) return;
        gst_element_set_state(pipeline_, GST_STATE_PAUSED);
        playing_ = false;
    }

    ~GStreamerCapture()
    {
        if (!pipeline_) return;
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
    }

private:
    static GstFlowReturn new_sample(GstAppSink *sink, gpointer opaque)
    {
        (void)opaque;
        GstSample *sample = gst_app_sink_pull_sample(sink);
        if (!sample) return GST_FLOW_ERROR;
        GstCaps *caps = gst_sample_get_caps(sample);
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstVideoInfo info;
        GstVideoFrame video_frame;
        gst_video_info_init(&info);
        if (caps && buffer && gst_video_info_from_caps(&info, caps) &&
            gst_video_frame_map(&video_frame, &info, buffer, GST_MAP_READ)) {
            publish_bgra(static_cast<const uint8_t *>(
                             GST_VIDEO_FRAME_PLANE_DATA(&video_frame, 0)),
                         GST_VIDEO_FRAME_WIDTH(&video_frame),
                         GST_VIDEO_FRAME_HEIGHT(&video_frame),
                         GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, 0));
            gst_video_frame_unmap(&video_frame);
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    static GstBusSyncReply bus_message(GstBus *, GstMessage *message, gpointer)
    {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError *error = nullptr;
            gchar *details = nullptr;
            gst_message_parse_error(message, &error, &details);
            fprintf(stderr, "uu-wayland-capture-helper: GStreamer: %s\n",
                    error ? error->message : "unknown error");
            if (error) g_error_free(error);
            g_free(details);
        }
        return GST_BUS_PASS;
    }

    GstElement *pipeline_ = nullptr;
    GstElement *sink_ = nullptr;
    QMutex state_mutex_;
    bool playing_ = false;
};

static GStreamerCapture *gstreamer_capture_instance;

class InputBridge {
public:
    uint32_t send(const CaptureInputEvent *events, uint32_t count)
    {
        QMutexLocker locker(&mutex_);
        bool use_eis = ensure_eis();
        if (!use_eis && !ensure_uinput()) return 0;
        uint32_t accepted = 0;
        for (; accepted < count; ++accepted) {
            bool ok = events[accepted].type == CAPTURE_INPUT_MOUSE
                ? (use_eis ? send_mouse_eis(events[accepted])
                           : send_mouse_uinput(events[accepted]))
                : events[accepted].type == CAPTURE_INPUT_KEYBOARD
                    ? (use_eis ? send_keyboard_eis(events[accepted])
                               : send_keyboard_uinput(events[accepted])) : false;
            if (!ok) break;
        }
        return accepted;
    }

    ~InputBridge()
    {
        reset_eis();
        destroy_device(&mouse_fd_);
        destroy_device(&keyboard_fd_);
    }

private:
    static void release_device(struct ei_device **device)
    {
        if (*device) *device = ei_device_unref(*device);
    }

    void clear_eis_device(struct ei_device *device)
    {
        struct ei_device **device_slots[] = {
            &pointer_device_, &absolute_device_, &button_device_,
            &scroll_device_, &keyboard_device_
        };
        for (struct ei_device **slot : device_slots) {
            if (*slot == device) release_device(slot);
        }
    }

    void reset_eis()
    {
        release_device(&pointer_device_);
        release_device(&absolute_device_);
        release_device(&button_device_);
        release_device(&scroll_device_);
        release_device(&keyboard_device_);
        if (eis_) eis_ = ei_unref(eis_);
    }

    static void set_eis_device(struct ei_device **slot, struct ei_device *device,
                               enum ei_device_capability capability)
    {
        if (!*slot && ei_device_has_capability(device, capability))
            *slot = ei_device_ref(device);
    }

    bool dispatch_eis(int timeout_ms)
    {
        struct pollfd poll_fd;
        bool disconnected = false;
        if (!eis_) return false;
        poll_fd.fd = ei_get_fd(eis_);
        poll_fd.events = POLLIN;
        poll_fd.revents = 0;
        if (poll(&poll_fd, 1, timeout_ms) < 0 && errno != EINTR) return false;
        ei_dispatch(eis_);
        while (struct ei_event *event = ei_get_event(eis_)) {
            enum ei_event_type type = ei_event_get_type(event);
            struct ei_device *device = ei_event_get_device(event);
            if (type == EI_EVENT_SEAT_ADDED) {
                struct ei_seat *seat = ei_event_get_seat(event);
                ei_seat_bind_capabilities(
                    seat, EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_POINTER_ABSOLUTE,
                    EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL,
                    EI_DEVICE_CAP_KEYBOARD, nullptr);
            } else if (type == EI_EVENT_DEVICE_RESUMED && device) {
                ei_device_start_emulating(device, 1);
                set_eis_device(&pointer_device_, device, EI_DEVICE_CAP_POINTER);
                set_eis_device(&absolute_device_, device,
                               EI_DEVICE_CAP_POINTER_ABSOLUTE);
                set_eis_device(&button_device_, device, EI_DEVICE_CAP_BUTTON);
                set_eis_device(&scroll_device_, device, EI_DEVICE_CAP_SCROLL);
                set_eis_device(&keyboard_device_, device, EI_DEVICE_CAP_KEYBOARD);
            } else if ((type == EI_EVENT_DEVICE_PAUSED ||
                        type == EI_EVENT_DEVICE_REMOVED) && device) {
                clear_eis_device(device);
            } else if (type == EI_EVENT_DISCONNECT) {
                disconnected = true;
            }
            ei_event_unref(event);
        }
        if (disconnected) {
            reset_eis();
            eis_retry_after_ = 0;
        }
        return !disconnected;
    }

    bool ensure_eis()
    {
        if (eis_) {
            dispatch_eis(0);
            return pointer_device_ && button_device_ && keyboard_device_;
        }
        if (time(nullptr) < eis_retry_after_) return false;
        eis_retry_after_ = time(nullptr) + 5;

        QDBusInterface remote_desktop(
            QStringLiteral("org.kde.KWin"),
            QStringLiteral("/org/kde/KWin/EIS/RemoteDesktop"),
            QStringLiteral("org.kde.KWin.EIS.RemoteDesktop"),
            QDBusConnection::sessionBus());
        QDBusMessage response = remote_desktop.call(
            QStringLiteral("connectToEIS"), 3);
        if (response.type() == QDBusMessage::ErrorMessage ||
            response.arguments().size() < 2) return false;
        QDBusUnixFileDescriptor descriptor =
            qvariant_cast<QDBusUnixFileDescriptor>(response.arguments().at(0));
        int fd = dup(descriptor.fileDescriptor());
        if (fd < 0) return false;

        eis_ = ei_new_sender(nullptr);
        if (!eis_) {
            close(fd);
            return false;
        }
        ei_configure_name(eis_, "UU Remote Wayland Input Bridge");
        if (ei_setup_backend_fd(eis_, fd) < 0) {
            reset_eis();
            return false;
        }
        for (int attempt = 0; attempt < 40 &&
             (!pointer_device_ || !button_device_ || !keyboard_device_); ++attempt)
            if (!dispatch_eis(50)) break;
        if (!pointer_device_ || !button_device_ || !keyboard_device_) {
            reset_eis();
            return false;
        }
        fprintf(stderr, "uu-wayland-capture-helper: KWin EIS input ready\n");
        eis_retry_after_ = 0;
        return true;
    }

    static bool set_bit(int fd, unsigned long request, int value)
    {
        return ioctl(fd, request, value) >= 0;
    }

    static int create_keyboard()
    {
        int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0 || !set_bit(fd, UI_SET_EVBIT, EV_KEY)) goto fail;
        for (int code = 1; code < BTN_MISC; ++code)
            if (!set_bit(fd, UI_SET_KEYBIT, code)) goto fail;
        {
            struct uinput_setup setup = {};
            setup.id.bustype = BUS_USB;
            setup.id.vendor = 0x1209;
            setup.id.product = 0x5551;
            strncpy(setup.name, "UU Remote Wayland Keyboard", UINPUT_MAX_NAME_SIZE - 1);
            if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0)
                goto fail;
        }
        return fd;
fail:
        if (fd >= 0) close(fd);
        return -1;
    }

    static int create_mouse()
    {
        int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        const int buttons[] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE,
                               BTN_EXTRA, BTN_FORWARD, BTN_BACK, BTN_TASK};
        const int relative_axes[] = {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL};
        if (fd < 0 || !set_bit(fd, UI_SET_EVBIT, EV_KEY) ||
            !set_bit(fd, UI_SET_EVBIT, EV_REL) ||
            !set_bit(fd, UI_SET_EVBIT, EV_ABS) ||
            !set_bit(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER)) goto fail;
        for (int button : buttons)
            if (!set_bit(fd, UI_SET_KEYBIT, button)) goto fail;
        for (int axis : relative_axes)
            if (!set_bit(fd, UI_SET_RELBIT, axis)) goto fail;
        for (int axis : {ABS_X, ABS_Y}) {
            struct uinput_abs_setup absolute = {};
            absolute.code = static_cast<__u16>(axis);
            absolute.absinfo.minimum = 0;
            absolute.absinfo.maximum = 65535;
            if (ioctl(fd, UI_ABS_SETUP, &absolute) < 0) goto fail;
        }
        {
            struct uinput_setup setup = {};
            setup.id.bustype = BUS_USB;
            setup.id.vendor = 0x1209;
            setup.id.product = 0x5552;
            strncpy(setup.name, "UU Remote Wayland Mouse", UINPUT_MAX_NAME_SIZE - 1);
            if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0)
                goto fail;
        }
        return fd;
fail:
        if (fd >= 0) close(fd);
        return -1;
    }

    static void destroy_device(int *fd)
    {
        if (*fd < 0) return;
        ioctl(*fd, UI_DEV_DESTROY);
        close(*fd);
        *fd = -1;
    }

    bool ensure_uinput()
    {
        if (keyboard_fd_ >= 0 && mouse_fd_ >= 0) return true;
        destroy_device(&keyboard_fd_);
        destroy_device(&mouse_fd_);
        keyboard_fd_ = create_keyboard();
        mouse_fd_ = create_mouse();
        if (keyboard_fd_ < 0 || mouse_fd_ < 0) {
            fprintf(stderr, "uu-wayland-capture-helper: cannot create uinput devices: %s\n",
                    strerror(errno));
            destroy_device(&keyboard_fd_);
            destroy_device(&mouse_fd_);
            return false;
        }
        fprintf(stderr, "uu-wayland-capture-helper: uinput keyboard and mouse ready\n");
        return true;
    }

    static bool emit_event(int fd, uint16_t type, uint16_t code, int32_t value)
    {
        struct input_event event = {};
        event.type = type;
        event.code = code;
        event.value = value;
        return write(fd, &event, sizeof(event)) == static_cast<ssize_t>(sizeof(event));
    }

    static int wheel_value(uint32_t data)
    {
        int32_t signed_value = static_cast<int32_t>(data);
        int result = signed_value / 120;
        if (!result && signed_value) result = signed_value > 0 ? 1 : -1;
        return result;
    }

    bool send_mouse_uinput(const CaptureInputEvent &event)
    {
        constexpr uint32_t move = 0x0001;
        constexpr uint32_t left_down = 0x0002;
        constexpr uint32_t left_up = 0x0004;
        constexpr uint32_t right_down = 0x0008;
        constexpr uint32_t right_up = 0x0010;
        constexpr uint32_t middle_down = 0x0020;
        constexpr uint32_t middle_up = 0x0040;
        constexpr uint32_t x_down = 0x0080;
        constexpr uint32_t x_up = 0x0100;
        constexpr uint32_t wheel = 0x0800;
        constexpr uint32_t horizontal_wheel = 0x1000;
        constexpr uint32_t absolute = 0x8000;
        bool ok = true;

        if (event.flags & move) {
            if (event.flags & absolute) {
                ok = emit_event(mouse_fd_, EV_ABS, ABS_X, event.dx) &&
                     emit_event(mouse_fd_, EV_ABS, ABS_Y, event.dy) && ok;
            } else {
                ok = emit_event(mouse_fd_, EV_REL, REL_X, event.dx) &&
                     emit_event(mouse_fd_, EV_REL, REL_Y, event.dy) && ok;
            }
        }
        if (event.flags & left_down) ok = emit_event(mouse_fd_, EV_KEY, BTN_LEFT, 1) && ok;
        if (event.flags & left_up) ok = emit_event(mouse_fd_, EV_KEY, BTN_LEFT, 0) && ok;
        if (event.flags & right_down) ok = emit_event(mouse_fd_, EV_KEY, BTN_RIGHT, 1) && ok;
        if (event.flags & right_up) ok = emit_event(mouse_fd_, EV_KEY, BTN_RIGHT, 0) && ok;
        if (event.flags & middle_down) ok = emit_event(mouse_fd_, EV_KEY, BTN_MIDDLE, 1) && ok;
        if (event.flags & middle_up) ok = emit_event(mouse_fd_, EV_KEY, BTN_MIDDLE, 0) && ok;
        if (event.flags & x_down) {
            int button = (event.mouse_data >> 16) == 1 ? BTN_SIDE : BTN_EXTRA;
            ok = emit_event(mouse_fd_, EV_KEY, button, 1) && ok;
        }
        if (event.flags & x_up) {
            int button = (event.mouse_data >> 16) == 1 ? BTN_SIDE : BTN_EXTRA;
            ok = emit_event(mouse_fd_, EV_KEY, button, 0) && ok;
        }
        if (event.flags & wheel)
            ok = emit_event(mouse_fd_, EV_REL, REL_WHEEL,
                            wheel_value(event.mouse_data)) && ok;
        if (event.flags & horizontal_wheel)
            ok = emit_event(mouse_fd_, EV_REL, REL_HWHEEL,
                            wheel_value(event.mouse_data)) && ok;
        return emit_event(mouse_fd_, EV_SYN, SYN_REPORT, 0) && ok;
    }

    static int extended_scan_code(uint16_t scan)
    {
        switch (scan & 0xff) {
        case 0x1c: return KEY_KPENTER;
        case 0x1d: return KEY_RIGHTCTRL;
        case 0x35: return KEY_KPSLASH;
        case 0x37: return KEY_SYSRQ;
        case 0x38: return KEY_RIGHTALT;
        case 0x47: return KEY_HOME;
        case 0x48: return KEY_UP;
        case 0x49: return KEY_PAGEUP;
        case 0x4b: return KEY_LEFT;
        case 0x4d: return KEY_RIGHT;
        case 0x4f: return KEY_END;
        case 0x50: return KEY_DOWN;
        case 0x51: return KEY_PAGEDOWN;
        case 0x52: return KEY_INSERT;
        case 0x53: return KEY_DELETE;
        case 0x5b: return KEY_LEFTMETA;
        case 0x5c: return KEY_RIGHTMETA;
        case 0x5d: return KEY_COMPOSE;
        default: return 0;
        }
    }

    static int virtual_key_code(uint16_t key)
    {
        static const int letters[] = {
            KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
            KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
            KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
        static const int digits[] = {KEY_0, KEY_1, KEY_2, KEY_3, KEY_4,
                                     KEY_5, KEY_6, KEY_7, KEY_8, KEY_9};
        static const int keypad[] = {KEY_KP0, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4,
                                     KEY_KP5, KEY_KP6, KEY_KP7, KEY_KP8, KEY_KP9};
        if (key >= 'A' && key <= 'Z') return letters[key - 'A'];
        if (key >= '0' && key <= '9') return digits[key - '0'];
        if (key >= 0x60 && key <= 0x69) return keypad[key - 0x60];
        if (key >= 0x70 && key <= 0x79) return KEY_F1 + key - 0x70;
        if (key == 0x7a) return KEY_F11;
        if (key == 0x7b) return KEY_F12;
        switch (key) {
        case 0x08: return KEY_BACKSPACE;
        case 0x09: return KEY_TAB;
        case 0x0d: return KEY_ENTER;
        case 0x10: case 0xa0: return KEY_LEFTSHIFT;
        case 0xa1: return KEY_RIGHTSHIFT;
        case 0x11: case 0xa2: return KEY_LEFTCTRL;
        case 0xa3: return KEY_RIGHTCTRL;
        case 0x12: case 0xa4: return KEY_LEFTALT;
        case 0xa5: return KEY_RIGHTALT;
        case 0x13: return KEY_PAUSE;
        case 0x14: return KEY_CAPSLOCK;
        case 0x1b: return KEY_ESC;
        case 0x20: return KEY_SPACE;
        case 0x21: return KEY_PAGEUP;
        case 0x22: return KEY_PAGEDOWN;
        case 0x23: return KEY_END;
        case 0x24: return KEY_HOME;
        case 0x25: return KEY_LEFT;
        case 0x26: return KEY_UP;
        case 0x27: return KEY_RIGHT;
        case 0x28: return KEY_DOWN;
        case 0x2c: return KEY_SYSRQ;
        case 0x2d: return KEY_INSERT;
        case 0x2e: return KEY_DELETE;
        case 0x5b: return KEY_LEFTMETA;
        case 0x5c: return KEY_RIGHTMETA;
        case 0x5d: return KEY_COMPOSE;
        case 0x6a: return KEY_KPASTERISK;
        case 0x6b: return KEY_KPPLUS;
        case 0x6d: return KEY_KPMINUS;
        case 0x6e: return KEY_KPDOT;
        case 0x6f: return KEY_KPSLASH;
        case 0x90: return KEY_NUMLOCK;
        case 0x91: return KEY_SCROLLLOCK;
        case 0xba: return KEY_SEMICOLON;
        case 0xbb: return KEY_EQUAL;
        case 0xbc: return KEY_COMMA;
        case 0xbd: return KEY_MINUS;
        case 0xbe: return KEY_DOT;
        case 0xbf: return KEY_SLASH;
        case 0xc0: return KEY_GRAVE;
        case 0xdb: return KEY_LEFTBRACE;
        case 0xdc: return KEY_BACKSLASH;
        case 0xdd: return KEY_RIGHTBRACE;
        case 0xde: return KEY_APOSTROPHE;
        default: return 0;
        }
    }

    bool send_keyboard_uinput(const CaptureInputEvent &event)
    {
        constexpr uint32_t extended = 0x0001;
        constexpr uint32_t key_up = 0x0002;
        constexpr uint32_t unicode = 0x0004;
        constexpr uint32_t scan_code = 0x0008;
        int code;
        if (event.flags & unicode) return false;
        if (event.flags & scan_code) {
            code = event.flags & extended ? extended_scan_code(event.scan_code)
                                          : event.scan_code & 0xff;
        } else {
            code = virtual_key_code(event.virtual_key);
        }
        if (code <= 0 || code >= BTN_MISC) return false;
        return emit_event(keyboard_fd_, EV_KEY, static_cast<uint16_t>(code),
                          event.flags & key_up ? 0 : 1) &&
               emit_event(keyboard_fd_, EV_SYN, SYN_REPORT, 0);
    }

    static void frame_eis(struct ei *context, struct ei_device *device)
    {
        ei_device_frame(device, ei_now(context));
    }

    bool send_mouse_eis(const CaptureInputEvent &event)
    {
        constexpr uint32_t move = 0x0001;
        constexpr uint32_t left_down = 0x0002;
        constexpr uint32_t left_up = 0x0004;
        constexpr uint32_t right_down = 0x0008;
        constexpr uint32_t right_up = 0x0010;
        constexpr uint32_t middle_down = 0x0020;
        constexpr uint32_t middle_up = 0x0040;
        constexpr uint32_t x_down = 0x0080;
        constexpr uint32_t x_up = 0x0100;
        constexpr uint32_t wheel = 0x0800;
        constexpr uint32_t horizontal_wheel = 0x1000;
        constexpr uint32_t absolute = 0x8000;

        dispatch_eis(0);
        if (!eis_) return false;
        if (event.flags & move) {
            if (event.flags & absolute) {
                struct ei_region *region;
                int32_t normalized_x = event.dx < 0 ? 0 :
                    event.dx > 65535 ? 65535 : event.dx;
                int32_t normalized_y = event.dy < 0 ? 0 :
                    event.dy > 65535 ? 65535 : event.dy;
                if (!absolute_device_ ||
                    !(region = ei_device_get_region(absolute_device_, 0))) return false;
                double x = ei_region_get_x(region) +
                    normalized_x * ei_region_get_width(region) / 65535.0;
                double y = ei_region_get_y(region) +
                    normalized_y * ei_region_get_height(region) / 65535.0;
                ei_device_pointer_motion_absolute(absolute_device_, x, y);
                frame_eis(eis_, absolute_device_);
            } else {
                if (!pointer_device_) return false;
                ei_device_pointer_motion(pointer_device_, event.dx, event.dy);
                frame_eis(eis_, pointer_device_);
            }
        }
        auto button = [this](uint32_t flag, int code, bool pressed) {
            if (!(flag)) return true;
            if (!button_device_) return false;
            ei_device_button_button(button_device_, code, pressed);
            frame_eis(eis_, button_device_);
            return true;
        };
        if (!button(event.flags & left_down, BTN_LEFT, true) ||
            !button(event.flags & left_up, BTN_LEFT, false) ||
            !button(event.flags & right_down, BTN_RIGHT, true) ||
            !button(event.flags & right_up, BTN_RIGHT, false) ||
            !button(event.flags & middle_down, BTN_MIDDLE, true) ||
            !button(event.flags & middle_up, BTN_MIDDLE, false)) return false;
        if (event.flags & (x_down | x_up)) {
            int code = (event.mouse_data >> 16) == 1 ? BTN_SIDE : BTN_EXTRA;
            if (!button(event.flags & x_down, code, true) ||
                !button(event.flags & x_up, code, false)) return false;
        }
        if (event.flags & (wheel | horizontal_wheel)) {
            if (!scroll_device_) return false;
            int amount = wheel_value(event.mouse_data) * 120;
            ei_device_scroll_discrete(
                scroll_device_, event.flags & horizontal_wheel ? amount : 0,
                event.flags & wheel ? -amount : 0);
            frame_eis(eis_, scroll_device_);
        }
        return true;
    }

    bool send_keyboard_eis(const CaptureInputEvent &event)
    {
        constexpr uint32_t extended = 0x0001;
        constexpr uint32_t key_up = 0x0002;
        constexpr uint32_t unicode = 0x0004;
        constexpr uint32_t scan_code = 0x0008;
        int code;
        dispatch_eis(0);
        if (!eis_ || !keyboard_device_ || event.flags & unicode) return false;
        if (event.flags & scan_code) {
            code = event.flags & extended ? extended_scan_code(event.scan_code)
                                          : event.scan_code & 0xff;
        } else {
            code = virtual_key_code(event.virtual_key);
        }
        if (code <= 0 || code >= BTN_MISC) return false;
        ei_device_keyboard_key(keyboard_device_, static_cast<uint32_t>(code),
                               !(event.flags & key_up));
        frame_eis(eis_, keyboard_device_);
        return true;
    }

    QMutex mutex_;
    time_t eis_retry_after_ = 0;
    struct ei *eis_ = nullptr;
    struct ei_device *pointer_device_ = nullptr;
    struct ei_device *absolute_device_ = nullptr;
    struct ei_device *button_device_ = nullptr;
    struct ei_device *scroll_device_ = nullptr;
    struct ei_device *keyboard_device_ = nullptr;
    int keyboard_fd_ = -1;
    int mouse_fd_ = -1;
};

static InputBridge input_bridge;

static int recv_all(int fd, void *data, size_t size)
{
    uint8_t *cursor = static_cast<uint8_t *>(data);
    while (size) {
        ssize_t count = recv(fd, cursor, size, 0);
        if (count == 0) return 0;
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        cursor += count;
        size -= static_cast<size_t>(count);
    }
    return 1;
}

static int send_all(int fd, const void *data, size_t size)
{
    const uint8_t *cursor = static_cast<const uint8_t *>(data);
    while (size) {
        ssize_t count = send(fd, cursor, size, MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        cursor += count;
        size -= static_cast<size_t>(count);
    }
    return 0;
}

static int authenticate_connection(int fd)
{
    const QByteArray token = qgetenv("UU_CAPTURE_AUTH_TOKEN");
    CaptureAuthRequest request = {};
    CaptureAuthReply reply;
    if (token.isEmpty()) return 0;
    if (token.size() != CAPTURE_AUTH_TOKEN_SIZE) return -1;
    request.magic = CAPTURE_MAGIC;
    request.type = CAPTURE_AUTH_REQUEST;
    request.protocol_version = CAPTURE_PROTOCOL_VERSION;
    request.token_size = CAPTURE_AUTH_TOKEN_SIZE;
    memcpy(request.token, token.constData(), CAPTURE_AUTH_TOKEN_SIZE);
    if (send_all(fd, &request, sizeof(request)) < 0 ||
        recv_all(fd, &reply, sizeof(reply)) <= 0 ||
        reply.magic != CAPTURE_MAGIC || reply.type != CAPTURE_AUTH_REPLY ||
        reply.protocol_version != CAPTURE_PROTOCOL_VERSION || reply.status)
        return -1;
    return 0;
}

static bool auth_token_equal(const char *token)
{
    unsigned int difference = 0;
    if (required_auth_token.size() != CAPTURE_AUTH_TOKEN_SIZE) return false;
    for (uint32_t index = 0; index < CAPTURE_AUTH_TOKEN_SIZE; ++index)
        difference |= static_cast<unsigned char>(token[index]) ^
            static_cast<unsigned char>(required_auth_token.at(index));
    return difference == 0;
}

static int wait_for_frame(unsigned long port, unsigned long timeout_seconds)
{
    const time_t deadline = time(nullptr) + static_cast<time_t>(timeout_seconds);
    const CaptureRequest request = {CAPTURE_MAGIC, CAPTURE_REQUEST,
                                    CAPTURE_PROTOCOL_VERSION, 0};

    do {
        struct sockaddr_in address;
        struct timeval timeout = {4, 0};
        CaptureReply reply;
        int connection = socket(AF_INET, SOCK_STREAM, 0);
        if (connection >= 0) {
            setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            memset(&address, 0, sizeof(address));
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(static_cast<uint16_t>(port));
            if (connect(connection, reinterpret_cast<struct sockaddr *>(&address),
                        sizeof(address)) == 0 && authenticate_connection(connection) == 0 &&
                send_all(connection, &request, sizeof(request)) == 0 &&
                recv_all(connection, &reply, sizeof(reply)) > 0 &&
                reply.magic == CAPTURE_MAGIC && reply.type == CAPTURE_REPLY &&
                reply.protocol_version == CAPTURE_PROTOCOL_VERSION && !reply.status &&
                reply.width && reply.height && reply.stride == reply.width * 4u &&
                reply.payload_size == reply.stride * reply.height) {
                close(connection);
                return 0;
            }
            close(connection);
        }
        usleep(250000);
    } while (time(nullptr) < deadline);

    fprintf(stderr, "uu-wayland-capture-helper: no frame available within %lu seconds\n",
            timeout_seconds);
    return 1;
}

static int wait_for_listener(unsigned long port, unsigned long timeout_seconds)
{
    const time_t deadline = time(nullptr) + static_cast<time_t>(timeout_seconds);
    do {
        int connection = socket(AF_INET, SOCK_STREAM, 0);
        if (connection >= 0) {
            struct sockaddr_in address = {};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(static_cast<uint16_t>(port));
            if (connect(connection, reinterpret_cast<struct sockaddr *>(&address),
                        sizeof(address)) == 0) {
                close(connection);
                return 0;
            }
            close(connection);
        }
        sleep(1);
    } while (time(nullptr) < deadline);
    fprintf(stderr, "uu-wayland-capture-helper: listener unavailable within %lu seconds\n",
            timeout_seconds);
    return 1;
}

static void handle_client(int client)
{
    bool authenticated = required_auth_token.isEmpty();
    for (;;) {
        CaptureRequest request;
        int receive_result = recv_all(client, &request, sizeof(request));
        if (receive_result <= 0) break;
        if (request.magic != CAPTURE_MAGIC ||
            request.protocol_version != CAPTURE_PROTOCOL_VERSION) break;
        if (request.type == CAPTURE_AUTH_REQUEST) {
            CaptureAuthRequest auth_request = {};
            CaptureAuthReply reply = {CAPTURE_MAGIC, CAPTURE_AUTH_REPLY,
                                      CAPTURE_PROTOCOL_VERSION, 1};
            auth_request.magic = request.magic;
            auth_request.type = request.type;
            auth_request.protocol_version = request.protocol_version;
            auth_request.token_size = request.reserved;
            if (auth_request.token_size != CAPTURE_AUTH_TOKEN_SIZE ||
                recv_all(client, auth_request.token,
                         sizeof(auth_request.token)) <= 0) break;
            authenticated = auth_token_equal(auth_request.token);
            reply.status = authenticated ? 0 : 1;
            memset(auth_request.token, 0, sizeof(auth_request.token));
            if (send_all(client, &reply, sizeof(reply)) < 0 || !authenticated) break;
            continue;
        }
        if (!authenticated) break;
        if (request.type == CAPTURE_REQUEST) {
            CaptureReply reply = {CAPTURE_MAGIC, CAPTURE_REPLY, CAPTURE_PROTOCOL_VERSION,
                                  1, 0, 0, 0, 0};
            QByteArray pixels;
            /* Keep serving the last complete frame when a portal capture
               tick is late. Returning an empty frame makes the Wine BitBlt
               hook fall back to GDI and causes visible flicker during quality
               changes or transient compositor stalls. */
            if (kwin_capture_on_request)
                capture_kwin_workspace();
            else if (!gstreamer_capture_instance || !gstreamer_capture_instance->resume())
                break;
            {
                QMutexLocker locker(&frame_state.mutex);
                if (frame_state.pixels.isEmpty())
                    frame_state.ready.wait(&frame_state.mutex, 3000);
                if (!frame_state.pixels.isEmpty()) {
                    pixels = frame_state.pixels;
                    reply.status = 0;
                    reply.width = frame_state.width;
                    reply.height = frame_state.height;
                    reply.stride = frame_state.width * 4u;
                    reply.payload_size = static_cast<uint32_t>(pixels.size());
                }
            }
            if (send_all(client, &reply, sizeof(reply)) < 0 ||
                (!pixels.isEmpty() &&
                 send_all(client, pixels.constData(), pixels.size()) < 0)) break;
        } else if (request.type == CAPTURE_INPUT_REQUEST) {
            CaptureInputEvent events[CAPTURE_MAX_INPUT_EVENTS];
            CaptureInputReply reply = {CAPTURE_MAGIC, CAPTURE_INPUT_REPLY,
                                       CAPTURE_PROTOCOL_VERSION, 1, 0};
            uint32_t count = request.reserved;
            if (!count || count > CAPTURE_MAX_INPUT_EVENTS ||
                recv_all(client, events, sizeof(events[0]) * count) <= 0) break;
            reply.accepted_count = input_bridge.send(events, count);
            reply.status = reply.accepted_count == count ? 0 : 1;
            if (send_all(client, &reply, sizeof(reply)) < 0) break;
        } else {
            break;
        }
    }
}

static void *client_thread(void *opaque)
{
    int client = static_cast<int>(reinterpret_cast<intptr_t>(opaque));
    active_client_count.fetch_add(1, std::memory_order_relaxed);
    handle_client(client);
    close(client);
    if (active_client_count.fetch_sub(1, std::memory_order_relaxed) == 1 &&
        gstreamer_capture_instance)
        gstreamer_capture_instance->suspend();
    return nullptr;
}

static void *server_thread(void *opaque)
{
    unsigned long port = reinterpret_cast<uintptr_t>(opaque);
    struct sockaddr_in address;
    int server;
    int reuse = 1;

    server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { perror("socket"); return nullptr; }
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(server, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) < 0 ||
        listen(server, 4) < 0) {
        perror("listen");
        close(server);
        return nullptr;
    }
    fprintf(stderr, "uu-wayland-capture-helper listening on 127.0.0.1:%lu\n", port);
    for (;;) {
        int client = accept(server, nullptr, nullptr);
        pthread_t thread;
        struct timeval send_timeout = {5, 0};
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        /* UU keeps separate capture and input sockets open for the whole
           session. An idle receive timeout turns the next key or click after
           five seconds into a stale-socket failure. EOF still releases the
           thread when the Wine process exits. */
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                   sizeof(send_timeout));
        if (pthread_create(&thread, nullptr, client_thread,
                           reinterpret_cast<void *>(static_cast<intptr_t>(client))) != 0) {
            close(client);
            continue;
        }
        pthread_detach(thread);
    }
    close(server);
    return nullptr;
}

int main(int argc, char **argv)
{
    unsigned long port = CAPTURE_DEFAULT_PORT;
    bool test_pattern = false;
    bool kwin_screenshot = false;
    pthread_t server;

    if (argc >= 2 && strcmp(argv[1], "--wait-ready") == 0) {
        unsigned long timeout_seconds = 120;
        if (argc >= 3) port = strtoul(argv[2], nullptr, 10);
        if (argc >= 4) timeout_seconds = strtoul(argv[3], nullptr, 10);
        if (!port || port > 65535 || !timeout_seconds) {
            fprintf(stderr, "invalid readiness arguments\n");
            return 2;
        }
        return wait_for_frame(port, timeout_seconds);
    }
    if (argc >= 2 && strcmp(argv[1], "--wait-listening") == 0) {
        unsigned long timeout_seconds = 120;
        if (argc >= 3) port = strtoul(argv[2], nullptr, 10);
        if (argc >= 4) timeout_seconds = strtoul(argv[3], nullptr, 10);
        if (!port || port > 65535 || !timeout_seconds) {
            fprintf(stderr, "invalid listener arguments\n");
            return 2;
        }
        return wait_for_listener(port, timeout_seconds);
    }

    int write_index = 1;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--test-pattern") == 0) {
            test_pattern = true;
        } else if (strcmp(argv[index], "--kwin-screenshot") == 0) {
            kwin_screenshot = true;
        } else {
            argv[write_index++] = argv[index];
        }
    }
    argc = write_index;
    argv[argc] = nullptr;
    if (test_pattern && kwin_screenshot) {
        fprintf(stderr, "capture modes are mutually exclusive\n");
        return 2;
    }
    if (argc == 2) {
        char *end = nullptr;
        port = strtoul(argv[1], &end, 10);
        if (!end || *end || !port || port > 65535) {
            fprintf(stderr, "invalid port\n");
            return 2;
        }
    }
    QCoreApplication::setApplicationName(QStringLiteral("uu-wayland-capture-helper"));
    QCoreApplication::setOrganizationName(QStringLiteral("uu-amf-bridge"));
    QGuiApplication::setDesktopFileName(QStringLiteral("uu-wayland-capture-helper"));
    QGuiApplication application(argc, argv);
    ScreenSaverInhibitor screen_saver_inhibitor;
    PortalCapture portal_capture;
    GStreamerCapture gstreamer_capture;
    gstreamer_capture_instance = &gstreamer_capture;
    required_auth_token = qgetenv("UU_CAPTURE_AUTH_TOKEN");
    if (!required_auth_token.isEmpty() &&
        required_auth_token.size() != CAPTURE_AUTH_TOKEN_SIZE) {
        fprintf(stderr, "UU_CAPTURE_AUTH_TOKEN must contain exactly %u bytes\n",
                CAPTURE_AUTH_TOKEN_SIZE);
        return 2;
    }
    kwin_capture_on_request = kwin_screenshot;
    signal(SIGPIPE, SIG_IGN);
    if (pthread_create(&server, nullptr, server_thread,
                       reinterpret_cast<void *>(static_cast<uintptr_t>(port))) != 0) {
        fprintf(stderr, "failed to start capture server\n");
        return 1;
    }
    pthread_detach(server);
    if (test_pattern) {
        QImage pattern(1920, 1080, QImage::Format_ARGB32);
        QPainter painter(&pattern);
        painter.fillRect(0, 0, 960, 540, QColor(220, 40, 40));
        painter.fillRect(960, 0, 960, 540, QColor(40, 200, 80));
        painter.fillRect(0, 540, 960, 540, QColor(40, 90, 220));
        painter.fillRect(960, 540, 960, 540, QColor(230, 190, 30));
        painter.end();
        publish_image(pattern);
        fprintf(stderr, "uu-wayland-capture-helper using synthetic test frame\n");
    } else if (kwin_screenshot) {
        if (!QDBusConnection::sessionBus().isConnected() ||
            !capture_kwin_workspace()) {
            fprintf(stderr, "uu-wayland-capture-helper: KWin capture unavailable\n");
            return 1;
        }
        fprintf(stderr, "uu-wayland-capture-helper using KWin greeter capture\n");
    } else {
        int pipewire_fd = -1;
        uint32_t node_id = 0;
        gst_init(&argc, &argv);
        if (!screen_saver_inhibitor.start() ||
            !portal_capture.start(&pipewire_fd, &node_id) ||
            !gstreamer_capture.start(pipewire_fd, node_id)) return 1;
    }
    return application.exec();
}

#include "wayland_capture_helper.moc"
