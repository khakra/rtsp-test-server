#include <string>
#include <deque>
#include <cstdlib>

#include <fmt/format.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>

#include "CxxPtr/GlibPtr.h"
#include "CxxPtr/GstRtspServerPtr.h"
#include "CxxPtr/libconfigDestroy.h"

#include "ConfigHelpers.h"

// Disable VA-API hardware acceleration to prevent crashes on systems
// without proper GPU support (e.g., nouveau driver issues, headless servers).
// This server only uses software encoders (x264, vp8) so VA-API is not needed.
static void DisableVaapi() {
    // Setting LIBVA_DRIVER_NAME to a non-existent driver prevents libva
    // from probing hardware drivers that may crash (like nouveau).
    // Using "null" as it's a recognized dummy driver name.
    setenv("LIBVA_DRIVER_NAME", "null", 0);
}

#define BARS  "bars"
#define WHITE "white"
#define BLACK "black"
#define RED   "red"
#define GREEN "green"
#define BLUE  "blue"
#define TEST "test"

static std::unique_ptr<spdlog::logger> Log;

struct Config {
    uint16_t port = 9554;
};

static void InitLogger() {
    spdlog::sink_ptr sink = std::make_shared<spdlog::sinks::stdout_sink_st>();

    Log = std::make_unique<spdlog::logger>("rtsp-test-server", sink);

    Log->set_level(spdlog::level::info);
}

static void LoadConfig(Config* config)
{
    const std::deque<std::string> configDirs = ::ConfigDirs();
    if(configDirs.empty())
        return;

    Config loadedConfig = *config;

    for(const std::string& configDir: configDirs) {
        const std::string configFile = configDir + "/rtsp-test-server.conf";
        if(!g_file_test(configFile.c_str(), G_FILE_TEST_IS_REGULAR)) {
            continue;
        }

        config_t config;
        config_init(&config);
        ConfigDestroy ConfigDestroy(&config);

        Log->error("Loading config \"{}\"", configFile);
        if(!config_read_file(&config, configFile.c_str())) {
            Log->error("Fail load config. {}. {}:{}",
                config_error_text(&config),
                configFile,
                config_error_line(&config));
            return;
        }

        int port = 0;
        if(CONFIG_TRUE == config_lookup_int(&config, "port", &port)) {
            if(port > 0 && port < 65535 ) {
                loadedConfig.port = port;
            } else {
                Log->error("Invalid port value");
            }
        }
    }

    *config = loadedConfig;
}

int main(int argc, char *argv[])
{
    InitLogger();

    Config config;
    LoadConfig(&config);

    const std::string h264PipelineTemplate =
        "( videotestsrc pattern={} ! "
     // "clockoverlay shaded-background=true font-desc=\"Sans, 36\" time-format=\"%D %H:%M:%S\" ! "
        "timeoverlay ! "
        "x264enc ! video/x-h264, profile=baseline ! "
        "rtph264pay name=pay0 pt=96 config-interval=-1 "
        "audiotestsrc ! alawenc ! rtppcmapay name=pay1 pt=8 )";

    const std::string vp8PipelineTemplate =
        "( videotestsrc pattern={} ! "
        "timeoverlay ! "
        "vp8enc ! rtpvp8pay name=pay0 pt=96 "
        "audiotestsrc ! opusenc ! rtpopuspay name=pay1 pt=97 )";

    const std::deque<std::pair<std::string, std::string>> createMountPoints = {
        /*
        {BARS, "smpte100"},
        {WHITE, "white"},
        {BLACK, "black"},
        {RED, "red"},
        {GREEN, "green"},
        {BLUE, "blue"},
        */
        {TEST, "smpte"},
    };

    DisableVaapi();
    gst_init(&argc, &argv);

    GstRTSPServerPtr staticServer(gst_rtsp_server_new());
    GstRTSPServer* server = staticServer.get();
    if(!server) {
        Log->error("Fail to create rtsp server");
        return -1;
    }

    GstRTSPMountPointsPtr mountPointsPtr(gst_rtsp_mount_points_new());
    GstRTSPMountPoints* mountPoints = mountPointsPtr.get();
    if(!server) {
        Log->error("Fail to create mount points");
        return -1;
    }

    gst_rtsp_server_set_service(server, std::to_string(config.port).c_str());

    gst_rtsp_server_set_mount_points(server, mountPoints);

    // h264
    for(const auto& mountPoint: createMountPoints) {
        GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_transport_mode(
            factory, GST_RTSP_TRANSPORT_MODE_PLAY);
        gst_rtsp_media_factory_set_launch(factory,
            fmt::format(h264PipelineTemplate, mountPoint.second).c_str());
        gst_rtsp_media_factory_set_shared(factory, TRUE);
        gst_rtsp_mount_points_add_factory(
            mountPoints,
            ("/" + mountPoint.first).c_str(),
            factory);
    }

    // vp8
    for(const auto& mountPoint: createMountPoints) {
        GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_transport_mode(
            factory, GST_RTSP_TRANSPORT_MODE_PLAY);
        gst_rtsp_media_factory_set_launch(factory,
            fmt::format(vp8PipelineTemplate, mountPoint.second).c_str());
        gst_rtsp_media_factory_set_shared(factory, TRUE);
        gst_rtsp_mount_points_add_factory(
            mountPoints,
            ("/" + mountPoint.first + "-vp8").c_str(),
            factory);
    }

    GMainLoopPtr loopPtr(g_main_loop_new(nullptr, FALSE));
    GMainLoop* loop = loopPtr.get();

    gst_rtsp_server_attach(server, nullptr);

    g_main_loop_run(loop);

    return 0;
}
