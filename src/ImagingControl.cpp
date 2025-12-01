#include "ImagingControl.hpp"

#include "Config.hpp"
#include "Logger.hpp"
#include "globals.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <cctype>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <imp/imp_isp.h>

#define MODULE "ImagingControl"

namespace
{
constexpr const char *kRuntimeDir = "/run/prudynt";
constexpr const char *kStatePath = "/run/prudynt/imaging.json";
constexpr const char *kFifoPath = "/run/prudynt/imagingctl";

struct FieldBinding
{
    const char *name;
    const char *config_path;
    int min_value;
    int max_value;
    int _image::*member;
    int (*apply_func)(int value);
};

constexpr int kBacklightMin = 0;
constexpr int kBacklightMax = 10;
constexpr int kWideDynamicRangeMin = 0;
constexpr int kWideDynamicRangeMax = 255;
constexpr int kToneMin = 0;
constexpr int kToneMax = 255;
constexpr int kDefogMin = 0;
constexpr int kDefogMax = 255;
#if defined(PLATFORM_T31) || defined(PLATFORM_C100) || defined(PLATFORM_T40) || defined(PLATFORM_T41) || defined(PLATFORM_T23)
constexpr int kNoiseReductionMin = 0;
constexpr int kNoiseReductionMax = 255;
#else
constexpr int kNoiseReductionMin = 50;
constexpr int kNoiseReductionMax = 150;
#endif

#if defined(NO_TUNINGS)
static int noop_apply(int value)
{
    (void) value;
    return 0;
}
#else
static int apply_brightness(int value)
{
    return IMP_ISP_Tuning_SetBrightness(value);
}

static int apply_contrast(int value)
{
    return IMP_ISP_Tuning_SetContrast(value);
}

static int apply_saturation(int value)
{
    return IMP_ISP_Tuning_SetSaturation(value);
}

static int apply_sharpness(int value)
{
    return IMP_ISP_Tuning_SetSharpness(value);
}
#endif

#if defined(NO_TUNINGS)
static const FieldBinding kFields[] = {
    {"brightness", "image.brightness", 0, 255, & _image::brightness, &noop_apply},
    {"contrast", "image.contrast", 0, 255, & _image::contrast, &noop_apply},
    {"saturation", "image.saturation", 0, 255, & _image::saturation, &noop_apply},
    {"sharpness", "image.sharpness", 0, 255, & _image::sharpness, &noop_apply},
    {"backlight", "image.backlight_compensation", kBacklightMin, kBacklightMax, & _image::backlight_compensation, nullptr},
    {"wide_dynamic_range", "image.drc_strength", kWideDynamicRangeMin, kWideDynamicRangeMax, & _image::drc_strength, nullptr},
    {"tone", "image.highlight_depress", kToneMin, kToneMax, & _image::highlight_depress, nullptr},
    {"defog", "image.defog_strength", kDefogMin, kDefogMax, & _image::defog_strength, nullptr},
    {"noise_reduction", "image.sinter_strength", kNoiseReductionMin, kNoiseReductionMax, & _image::sinter_strength, nullptr},
};
#else
static const FieldBinding kFields[] = {
    {"brightness", "image.brightness", 0, 255, & _image::brightness, &apply_brightness},
    {"contrast", "image.contrast", 0, 255, & _image::contrast, &apply_contrast},
    {"saturation", "image.saturation", 0, 255, & _image::saturation, &apply_saturation},
    {"sharpness", "image.sharpness", 0, 255, & _image::sharpness, &apply_sharpness},
    {"backlight", "image.backlight_compensation", kBacklightMin, kBacklightMax, & _image::backlight_compensation, nullptr},
    {"wide_dynamic_range", "image.drc_strength", kWideDynamicRangeMin, kWideDynamicRangeMax, & _image::drc_strength, nullptr},
    {"tone", "image.highlight_depress", kToneMin, kToneMax, & _image::highlight_depress, nullptr},
    {"defog", "image.defog_strength", kDefogMin, kDefogMax, & _image::defog_strength, nullptr},
    {"noise_reduction", "image.sinter_strength", kNoiseReductionMin, kNoiseReductionMax, & _image::sinter_strength, nullptr},
};
#endif

struct ParsedAssignment
{
    std::string key;
    double value{0.0};
    bool normalized{false};
};

std::atomic<bool> running{false};
std::atomic<bool> stop_requested{false};
std::thread fifo_thread;

bool ensure_runtime_dir()
{
    struct stat st
    {
    };
    if (stat(kRuntimeDir, &st) == 0)
    {
        if (S_ISDIR(st.st_mode))
            return true;
        if (unlink(kRuntimeDir) != 0)
        {
            LOG_WARN("Unable to remove stale runtime path: " << kRuntimeDir);
            return false;
        }
    }

    if (mkdir(kRuntimeDir, 0755) == 0)
        return true;

    if (errno == EEXIST)
        return true;

    LOG_WARN("Unable to create runtime directory: errno=" << errno);
    return false;
}

void ensure_fifo()
{
    if (mkfifo(kFifoPath, 0660) == -1 && errno != EEXIST)
    {
        LOG_WARN("Unable to create imaging control FIFO: errno=" << errno);
    }
    chmod(kFifoPath, 0660);
}

int clamp_to_range(const FieldBinding &binding, int value)
{
    return std::max(binding.min_value, std::min(binding.max_value, value));
}

bool write_state_snapshot()
{
    if (!cfg)
        return false;

    if (!ensure_runtime_dir())
        return false;

    const size_t field_count = sizeof(kFields) / sizeof(kFields[0]);
    std::vector<int> values(field_count, 0);
    {
        std::lock_guard<std::mutex> lock(cfg->configMutex);
        for (size_t i = 0; i < field_count; ++i)
        {
            values[i] = (cfg->image).*(kFields[i].member);
        }
    }

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();

    std::ostringstream oss;
    oss << "{\n  \"updated_at_ms\": " << now << ",\n  \"fields\": {\n";
    for (size_t i = 0; i < field_count; ++i)
    {
        const auto &binding = kFields[i];
        double normalized = 0.0;
        if (binding.max_value > binding.min_value)
        {
            normalized = static_cast<double>(values[i] - binding.min_value)
                / static_cast<double>(binding.max_value - binding.min_value);
        }
        oss << "    \"" << binding.name << "\": {\"value\": " << values[i]
            << ", \"min\": " << binding.min_value
            << ", \"max\": " << binding.max_value
            << ", \"normalized\": " << normalized << "}";
        if (i + 1 < field_count)
            oss << ',';
        oss << '\n';
    }
    oss << "  }\n}\n";

    std::string tmp_path = std::string(kStatePath) + ".tmp";
    int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        LOG_WARN("Unable to write imaging state: open failed errno=" << errno);
        return false;
    }

    const std::string payload = oss.str();
    ssize_t written = write(fd, payload.data(), payload.size());
    close(fd);
    if (written != static_cast<ssize_t>(payload.size()))
    {
        LOG_WARN("Unable to write imaging state: short write");
        unlink(tmp_path.c_str());
        return false;
    }

    if (rename(tmp_path.c_str(), kStatePath) != 0)
    {
        LOG_WARN("Unable to move imaging state file into place: errno=" << errno);
        unlink(tmp_path.c_str());
        return false;
    }

    return true;
}

const FieldBinding *find_field(const std::string &key)
{
    for (const auto &binding : kFields)
    {
        if (key == binding.name)
            return &binding;
    }
    return nullptr;
}

std::optional<ParsedAssignment> parse_assignment(const std::string &token)
{
    auto pos = token.find('=');
    if (pos == std::string::npos)
        return std::nullopt;

    ParsedAssignment result;
    result.key = token.substr(0, pos);
    std::transform(result.key.begin(), result.key.end(), result.key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    std::string raw_value = token.substr(pos + 1);
    bool percent = false;
    if (!raw_value.empty() && raw_value.back() == '%')
    {
        percent = true;
        raw_value.pop_back();
    }

    try
    {
        result.value = std::stod(raw_value);
    }
    catch (const std::exception &)
    {
        return std::nullopt;
    }

    if (percent)
    {
        result.value /= 100.0;
        result.normalized = true;
    }
    else if (raw_value.find('.') != std::string::npos)
    {
        if (result.value >= 0.0 && result.value <= 1.0)
            result.normalized = true;
    }

    return result;
}

int to_raw_value(const FieldBinding &binding, const ParsedAssignment &assignment)
{
    if (assignment.normalized)
    {
        double clamped = std::clamp(assignment.value, 0.0, 1.0);
        double span = static_cast<double>(binding.max_value - binding.min_value);
        return clamp_to_range(binding, static_cast<int>(std::round(clamped * span)) + binding.min_value);
    }

    return clamp_to_range(binding, static_cast<int>(std::round(assignment.value)));
}

bool apply_field(const FieldBinding &binding, int raw_value)
{
    if (!cfg)
        return false;

    if (binding.apply_func)
    {
        int rc = binding.apply_func(raw_value);
        if (rc != 0)
        {
            LOG_ERROR("Unable to apply " << binding.name << ": rc=" << rc);
            return false;
        }
    }

    if (!cfg->set<int>(binding.config_path, raw_value))
    {
        LOG_ERROR("CFG::set failed for " << binding.config_path);
        return false;
    }

    return true;
}

std::string trim(std::string value)
{
    auto not_space = [](unsigned char ch) {
        return !std::isspace(ch);
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

void handle_line(const std::string &line)
{
    std::string trimmed = trim(line);
    if (trimmed.empty())
        return;

    LOG_DEBUG("ImagingControl FIFO command: " << trimmed);

    std::istringstream iss(trimmed);
    std::string verb;
    iss >> verb;
    std::transform(verb.begin(), verb.end(), verb.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (verb != "set")
    {
        LOG_DEBUG("Ignoring unsupported imaging control verb: " << verb);
        return;
    }

    std::vector<std::pair<std::string, int>> persist_entries;
    bool updated = false;

    std::string token;
    while (iss >> token)
    {
        auto parsed = parse_assignment(token);
        if (!parsed)
            continue;

        const FieldBinding *binding = find_field(parsed->key);
        if (!binding)
        {
            LOG_DEBUG("Unknown imaging field: " << parsed->key);
            continue;
        }

        int raw_value = to_raw_value(*binding, *parsed);
        if (apply_field(*binding, raw_value))
        {
            persist_entries.emplace_back(binding->config_path, raw_value);
            updated = true;
        }
    }

    if (!updated)
        return;

    if (!cfg || !cfg->saveIntValues(persist_entries))
    {
        LOG_WARN("Failed to persist imaging changes to config file");
    }

    if (!write_state_snapshot())
    {
        LOG_WARN("Failed to refresh imaging state snapshot");
    }
}

void run_fifo_loop()
{
    LOG_INFO("ImagingControl FIFO loop starting");
    while (!stop_requested.load() && !global_shutdown_requested.load(std::memory_order_relaxed))
    {
        int fd = open(kFifoPath, O_RDONLY);
        if (fd < 0)
        {
            LOG_DEBUG("ImagingControl: waiting for FIFO (open failed errno=" << errno << ")");
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        LOG_INFO("ImagingControl: FIFO opened for reading");

        std::string buffer;
        buffer.reserve(256);
        char chunk[256];

        while (!stop_requested.load() && !global_shutdown_requested.load(std::memory_order_relaxed))
        {
            ssize_t bytes = read(fd, chunk, sizeof(chunk));
            if (bytes <= 0)
                break;

            buffer.append(chunk, static_cast<size_t>(bytes));
            size_t pos = 0;
            while ((pos = buffer.find('\n')) != std::string::npos)
            {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);
                handle_line(line);
            }
        }

        close(fd);
    }
}

} // namespace

void ImagingControl::start()
{
    bool expected = false;
    if (!running.compare_exchange_strong(expected, true))
        return;

    stop_requested.store(false);

    LOG_INFO("ImagingControl: start requested");

    if (!ensure_runtime_dir())
    {
        LOG_WARN("ImagingControl: unable to create runtime directory");
        running.store(false);
        return;
    }

    ensure_fifo();
    write_state_snapshot();

    LOG_INFO("ImagingControl: launching FIFO thread");

    fifo_thread = std::thread(run);
}

void ImagingControl::stop()
{
    if (!running.load())
        return;

    stop_requested.store(true);

    int fd = open(kFifoPath, O_WRONLY | O_NONBLOCK);
    if (fd >= 0)
    {
        const char newline = '\n';
        write(fd, &newline, 1);
        close(fd);
    }

    if (fifo_thread.joinable())
        fifo_thread.join();

    running.store(false);
}

bool ImagingControl::isRunning()
{
    return running.load();
}

void ImagingControl::refreshSnapshot()
{
    if (!write_state_snapshot())
    {
        LOG_DEBUG("ImagingControl: snapshot refresh skipped (writer unavailable)");
    }
}

void ImagingControl::run()
{
    LOG_INFO("ImagingControl thread entered run()");
    run_fifo_loop();
    LOG_INFO("ImagingControl thread exiting run()");
}
