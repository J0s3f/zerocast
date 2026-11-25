/*
 * SPDX-License-Identifier: MIT
 */

#include "zerocast_ui.h"

#include "bindings.h"
#include "linux_input.h"
#include "screenshot_service.h"
#include "theme.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace view {
namespace {

constexpr int kFooterHeight = 22;
constexpr int kContentHeight = kScreenHeight - kTitleBarHeight - kFooterHeight;
constexpr int kMaxVisibleRows = 5;
constexpr size_t kMaxEditLength = 200;
constexpr int kWebPort = 8080;
constexpr int kWebTtlSec = 300;

constexpr int kBitratesKbps[] = {1500, 2500, 4000, 6000};
struct Resolution {
    int w;
    int h;
};
constexpr Resolution kResolutions[] = {{1280, 720}, {854, 480}, {640, 360}};
constexpr int kFpsChoices[] = {20, 25, 30};

template <typename T, size_t N>
int index_of(const T (&values)[N], const T& value)
{
    for (size_t i = 0; i < N; ++i) {
        if (values[i] == value)
            return static_cast<int>(i);
    }
    return 0;
}

template <typename T, size_t N>
const T& cycled(const T (&values)[N], int current, int delta)
{
    const int n = static_cast<int>(N);
    return values[((current + delta) % n + n) % n];
}

// Words that only make a device name longer: company suffixes and serial numbers.
bool noise_word(const std::string& w)
{
    for (const char* n : {"Co.", "Co.,", "Ltd.", "Ltd", "Inc.", "Inc", "Corp.", "Corp", "Technology", "Technologies", "Electronics"})
        if (w == n)
            return true;
    size_t digits = 0;
    for (char c : w)
        digits += std::isdigit(static_cast<unsigned char>(c)) ? 1 : 0;
    return w.size() >= 4 && digits >= 4 && digits + 2 >= w.size();  // "SN0001", "0000000001"
}

// "Product: Product" (UVC drivers repeat themselves) -> "Product"; noise words removed.
std::string tidy_name(std::string name)
{
    for (char& c : name)
        if (c == '_')
            c = ' ';
    const size_t colon = name.find(": ");
    if (colon != std::string::npos && name.substr(0, colon) == name.substr(colon + 2))
        name.resize(colon);
    std::istringstream in(name);
    std::string word, out;
    while (in >> word) {
        if (noise_word(word))
            continue;
        out += (out.empty() ? "" : " ") + word;
    }
    return out.empty() ? name : out;
}

// Fits text into a list row.
std::string fit(const std::string& s, size_t room)
{
    return s.size() <= room ? s : s.substr(0, room - 1) + "~";
}

// Gives devices that would look identical a number ("1 Webcam", "2 Webcam"). `labels` are all the
// devices' tidy names, `index` the one being shown.
std::string numbered(const std::vector<std::string>& labels, size_t index, size_t room)
{
    int same = 0, rank = 0;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (labels[i] != labels[index])
            continue;
        ++same;
        if (i < index)
            ++rank;
    }
    if (same < 2)
        return fit(labels[index], room);
    const std::string prefix = std::to_string(rank + 1) + " ";
    return prefix + fit(labels[index], room - prefix.size());
}

// Name of a PulseAudio source for a list row, e.g.
// "alsa_input.usb-Sonix_Technology_Co.__Ltd._AUKEY_PC-W3S_SN0001-02.analog-stereo" -> "Sonix AUKEY PC-W3S".
std::string mic_name(const std::string& device)
{
    if (device == "@DEFAULT_SOURCE@" || device.empty())
        return "Default";
    std::string s = device;
    for (const char* prefix : {"alsa_input.", "bluez_input."}) {
        if (s.rfind(prefix, 0) == 0)
            s = s.substr(std::string(prefix).size());
    }
    // Drop the profile ("analog-stereo", "mono-fallback", ...): the last dot-separated part.
    const size_t last = s.rfind('.');
    if (last != std::string::npos) {
        const std::string tail = s.substr(last + 1);
        if (tail.find("analog") == 0 || tail.find("mono") == 0 || tail.find("stereo") == 0 || tail.find("iec958") == 0 ||
            tail.find("multichannel") == 0 || tail.find("input") == 0)
            s.resize(last);
    }
    for (const char* bus : {"usb-", "pci-", "platform-"})
        if (s.rfind(bus, 0) == 0)
            s = s.substr(std::string(bus).size());
    // Trailing "-00" interface index.
    if (s.size() > 3 && s[s.size() - 3] == '-' && std::isdigit(static_cast<unsigned char>(s[s.size() - 2])) &&
        std::isdigit(static_cast<unsigned char>(s[s.size() - 1])))
        s.resize(s.size() - 3);
    return tidy_name(s);
}

std::string mic_label(const std::string& device)
{
    std::vector<std::string> labels = {"Default"};
    size_t index = 0;
    for (const auto& name : stream::list_audio_inputs()) {
        labels.push_back(mic_name(name));
        if (name == device)
            index = labels.size() - 1;
    }
    if (device == "@DEFAULT_SOURCE@" || device.empty())
        index = 0;
    else if (index == 0) {
        labels.push_back(mic_name(device));  // chosen earlier, not connected now
        index = labels.size() - 1;
    }
    return numbered(labels, index, 22);
}

std::string clock_text(long seconds)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02ld:%02ld:%02ld", seconds / 3600, (seconds / 60) % 60, seconds % 60);
    return buf;
}

std::string minutes_text(int seconds)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", seconds / 60, seconds % 60);
    return buf;
}

const char* page_title(int page)
{
    static const char* titles[] = {"ZeroCast - Live", "ZeroCast - Sources", "ZeroCast - Send to",
                                   "ZeroCast - Settings"};
    return titles[page];
}

}  // namespace

ZeroCastUi::ZeroCastUi(viewmodel::BaseViewModel& view_model,
                       app::AssetManager& assets,
                       stream::Config config,
                       std::string config_path)
    : vm_(view_model), assets_(assets), cfg_(std::move(config)), cfg_path_(std::move(config_path))
{
    root_ = lv_obj_create(nullptr);
    lv_obj_remove_style_all(root_);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(root_, kScreenWidth, kScreenHeight);
    reactive::bind_theme(root_, vm_.dark_mode_subject(), reactive::ThemeRole::Screen);

    title_bar_ = std::make_unique<widgets::TitleBar>(root_, vm_, assets_);
    title_bar_->build();

    content_ = lv_obj_create(root_);
    lv_obj_remove_style_all(content_);
    lv_obj_clear_flag(content_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(content_, kScreenWidth, kContentHeight);
    lv_obj_align(content_, LV_ALIGN_TOP_MID, 0, kTitleBarHeight);
    reactive::bind_theme(content_, vm_.dark_mode_subject(), reactive::ThemeRole::Surface);

    footer_ = lv_obj_create(root_);
    lv_obj_remove_style_all(footer_);
    lv_obj_clear_flag(footer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(footer_, kScreenWidth, kFooterHeight);
    lv_obj_align(footer_, LV_ALIGN_BOTTOM_MID, 0, 0);
    reactive::bind_theme(footer_, vm_.dark_mode_subject(), reactive::ThemeRole::Bar);

    lv_screen_load(root_);

    toast_ = std::make_unique<widgets::Toast>(lv_layer_top(), vm_, assets_);
    toast_->build();

    platform::set_global_key_listener(key_cb, this);
    platform::set_key_release_listener(release_cb, this);

    // Background streaming: pick up a stream an earlier instance left running.
    engine_.set_keep_running(cfg_.background_stream);
    const bool reattached = engine_.attach(cfg_);

    set_page(Page::Live);
    timer_ = lv_timer_create(tick_cb, 100, this);
    if (reattached)
        toast_->show("Reconnected to the running stream", widgets::ToastTone::Success, 2600);
}

ZeroCastUi::~ZeroCastUi()
{
    platform::clear_key_release_listener(release_cb, this);
    platform::clear_global_key_listener(key_cb, this);
    if (timer_)
        lv_timer_delete(timer_);
    web_.stop();
    toast_.reset();
    title_bar_.reset();
    if (root_ && lv_obj_is_valid(root_))
        lv_obj_delete(root_);
}

bool ZeroCastUi::streaming() const
{
    const auto s = engine_.state();
    return s == stream::EngineState::Starting || s == stream::EngineState::Live ||
           s == stream::EngineState::Stopping;
}

// ---------------------------------------------------------------------------------------------
// Input
//
// Browse:  Up/Down (or K/J) select   Left/Right (or H/L) change   Enter/Space change or act
//          Tab / 1-4 switch page     G go live/stop              Esc back, hold Esc to exit
// Destinations: E edit key, U edit URL.   Edit overlay: type, Backspace, Enter save, Esc cancel,
// Tab show/hide.   PrintScreen: screenshot.
// ---------------------------------------------------------------------------------------------

bool ZeroCastUi::key_cb(uint32_t key, const char*, bool long_pressed, void* user_data)
{
    return static_cast<ZeroCastUi*>(user_data)->handle_key(key, long_pressed);
}

void ZeroCastUi::release_cb(uint32_t key, const char*, void* user_data)
{
    static_cast<ZeroCastUi*>(user_data)->handle_release(key);
}

bool ZeroCastUi::handle_key(uint32_t key, bool long_pressed)
{
    if (key == platform::kKeyPrintScreen) {
        if (!long_pressed)
            take_screenshot();
        return true;
    }

    // Esc is resolved on release so that a long press can mean "exit" without also going back.
    if (key == LV_KEY_ESC) {
        if (!long_pressed) {
            esc_down_ = true;
            esc_long_done_ = false;
        } else if (esc_down_ && !esc_long_done_) {
            esc_long_done_ = true;
            if (mode_ == Mode::Remote)
                stop_remote();
            else if (mode_ == Mode::EditText) {
                mode_ = Mode::Browse;
                refresh();
            } else
                quit_requested();
        }
        return true;
    }

    if (long_pressed)
        return true;  // other keys: nothing on long press

    switch (mode_) {
    case Mode::EditText: return handle_edit_key(key);
    case Mode::Remote: return true;  // only Esc (on release) does anything here
    case Mode::Browse: break;
    }
    return handle_browse_key(key);
}

void ZeroCastUi::handle_release(uint32_t key)
{
    if (key != LV_KEY_ESC || !esc_down_)
        return;
    esc_down_ = false;
    if (esc_long_done_)
        return;

    // Short Esc = back / cancel.
    if (mode_ == Mode::Remote) {
        stop_remote();
    } else if (mode_ == Mode::EditText) {
        mode_ = Mode::Browse;
        refresh();
    } else if (page_ != Page::Live) {
        set_page(Page::Live);
    }
}

bool ZeroCastUi::handle_edit_key(uint32_t key)
{
    if (key == LV_KEY_ENTER) {
        commit_edit();
    } else if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
        if (!edit_buf_.empty())
            edit_buf_.pop_back();
    } else if (key == '\t') {
        edit_reveal_ = !edit_reveal_;
    } else if (key >= 32 && key <= 126) {
        if (edit_buf_.size() < kMaxEditLength)
            edit_buf_ += static_cast<char>(key);
    }
    refresh();
    return true;
}

bool ZeroCastUi::handle_browse_key(uint32_t key)
{
    // The arrow cluster is printed on Z (left), F (up), X (down), C (right) and normally needs Fn.
    // Outside text entry the plain letters work as arrows too. (Text entry never gets here.)
    switch (key) {
    case 'z': key = LV_KEY_LEFT; break;
    case 'f': key = LV_KEY_UP; break;
    case 'x': key = LV_KEY_DOWN; break;
    case 'c': key = LV_KEY_RIGHT; break;
    default: break;
    }

    const bool on_live = page_ == Page::Live;
    const auto list = items(page_);
    const int count = static_cast<int>(list.size());
    int& sel = selection_[static_cast<int>(page_)];

    switch (key) {
    case '\t':
        set_page(static_cast<Page>((static_cast<int>(page_) + 1) % 4));
        return true;
    case '1': set_page(Page::Live); return true;
    case '2': set_page(Page::Sources); return true;
    case '3': set_page(Page::Destinations); return true;
    case '4': set_page(Page::Settings); return true;
    case 'g':
    case 'G':
        toggle_stream();
        return true;
    case LV_KEY_UP:
    case 'k':
        move_selection(-1);
        return true;
    case LV_KEY_DOWN:
    case 'j':
        move_selection(1);
        return true;
    case LV_KEY_LEFT:
    case 'h':
        if (on_live)
            set_page(static_cast<Page>((static_cast<int>(page_) + 3) % 4));
        else if (sel < count)
            change(list[sel], -1);
        return true;
    case LV_KEY_RIGHT:
    case 'l':
        if (on_live)
            set_page(static_cast<Page>((static_cast<int>(page_) + 1) % 4));
        else if (sel < count)
            change(list[sel], 1);
        return true;
    case LV_KEY_ENTER:
    case ' ':
        if (on_live)
            toggle_stream();
        else if (sel < count)
            change(list[sel], 1);
        return true;
    case 'e':
    case 'E':
        if (page_ == Page::Destinations && sel < count && list[sel].kind == Kind::Dest) {
            edit_dest_ = list[sel].index;
            begin_edit(EditField::Key);
        }
        return true;
    case 'u':
    case 'U':
        if (page_ == Page::Destinations && sel < count && list[sel].kind == Kind::Dest) {
            edit_dest_ = list[sel].index;
            begin_edit(EditField::Url);
        }
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------------

void ZeroCastUi::set_page(Page page)
{
    page_ = page;
    mode_ = Mode::Browse;
    vm_.set_title(page_title(static_cast<int>(page_)));
    refresh();
}

const ZeroCastUi::CameraFormats& ZeroCastUi::camera_formats() const
{
    std::string device = cfg_.sources.usb_device;
    if (device == "auto")
        device = stream::detect_usb_camera();
    if (device != probed_device_) {
        probed_device_ = device;
        probed_formats_ = {};
        if (!device.empty()) {
            for (const auto& m : stream::probe_usb_modes(device)) {
                probed_formats_.known = true;
                (m.format == "mjpeg" ? probed_formats_.mjpeg : probed_formats_.raw) = true;
            }
        }
    }
    return probed_formats_;
}

void ZeroCastUi::move_selection(int delta)
{
    const int count = static_cast<int>(items(page_).size());
    if (count == 0)
        return;
    int& sel = selection_[static_cast<int>(page_)];
    sel = ((sel + delta) % count + count) % count;
    refresh();
}

void ZeroCastUi::save_config()
{
    if (!stream::save_config(cfg_path_, cfg_))
        toast_->show("Could not save settings", widgets::ToastTone::Error, 3000);
}

void ZeroCastUi::change(const Item& item, int delta)
{
    switch (item.kind) {
    case Kind::Video: {
        // Camera -> USB camera -> Screen -> Camera (Left goes the other way).
        const int order[] = {static_cast<int>(stream::VideoSource::Camera), static_cast<int>(stream::VideoSource::Usb),
                             static_cast<int>(stream::VideoSource::Screen)};
        int current = 0;
        for (int i = 0; i < 3; ++i)
            if (order[i] == static_cast<int>(cfg_.sources.video))
                current = i;
        cfg_.sources.video = static_cast<stream::VideoSource>(order[((current + delta) % 3 + 3) % 3]);
        break;
    }
    case Kind::UsbFormat: {
        // Auto, then only what this camera actually offers.
        const CameraFormats& caps = camera_formats();
        std::vector<std::string> order = {"auto"};
        if (!caps.known || caps.mjpeg)
            order.push_back("mjpeg");
        if (!caps.known || caps.raw)
            order.push_back("raw");
        int current = 0;
        for (size_t i = 0; i < order.size(); ++i)
            if (cfg_.sources.usb_format == order[i])
                current = static_cast<int>(i);
        const int n = static_cast<int>(order.size());
        cfg_.sources.usb_format = order[((current + delta) % n + n) % n];
        break;
    }
    case Kind::UsbInfo: {
        // auto -> each connected camera in turn -> auto. With one camera plugged in it stays on auto.
        std::vector<std::string> choices = {"auto"};
        for (const auto& cam : stream::list_usb_cameras())
            choices.push_back(cam.device);
        int current = 0;
        for (size_t i = 0; i < choices.size(); ++i)
            if (choices[i] == cfg_.sources.usb_device)
                current = static_cast<int>(i);
        const int n = static_cast<int>(choices.size());
        cfg_.sources.usb_device = choices[((current + delta) % n + n) % n];
        break;
    }
    case Kind::MicInput: {
        std::vector<std::string> choices = {"@DEFAULT_SOURCE@"};
        for (const auto& name : stream::list_audio_inputs())
            choices.push_back(name);
        int current = 0;
        for (size_t i = 0; i < choices.size(); ++i)
            if (choices[i] == cfg_.sources.mic_device)
                current = static_cast<int>(i);
        const int n = static_cast<int>(choices.size());
        cfg_.sources.mic_device = choices[((current + delta) % n + n) % n];
        break;
    }
    case Kind::Screen:
        cfg_.sources.screen = cfg_.sources.screen == stream::ScreenTarget::Lcd ? stream::ScreenTarget::Hdmi
                                                                               : stream::ScreenTarget::Lcd;
        break;
    case Kind::Mic: cfg_.sources.mic = !cfg_.sources.mic; break;
    case Kind::System: cfg_.sources.system_audio = !cfg_.sources.system_audio; break;
    case Kind::LcdNative: cfg_.sources.lcd_native = !cfg_.sources.lcd_native; break;
    case Kind::Flip:
        if (cfg_.sources.video == stream::VideoSource::Usb)
            cfg_.sources.usb_rotate_180 = !cfg_.sources.usb_rotate_180;
        else
            cfg_.sources.camera_rotate_180 = !cfg_.sources.camera_rotate_180;
        break;
    case Kind::Dest:
        if (item.index >= 0 && item.index < static_cast<int>(cfg_.destinations.size())) {
            auto& d = cfg_.destinations[item.index];
            d.enabled = !d.enabled;
            if (d.enabled && d.key.empty())
                toast_->show(d.name + ": press E to enter its stream key", widgets::ToastTone::Warning, 2800);
        }
        break;
    case Kind::Remote:
        start_remote();
        return;
    case Kind::Bitrate:
        cfg_.video.bitrate_kbps = cycled(kBitratesKbps, index_of(kBitratesKbps, cfg_.video.bitrate_kbps), delta);
        break;
    case Kind::Resolution: {
        int current = 0;
        for (int i = 0; i < 3; ++i)
            if (kResolutions[i].w == cfg_.video.width)
                current = i;
        const Resolution next = cycled(kResolutions, current, delta);
        cfg_.video.width = next.w;
        cfg_.video.height = next.h;
        break;
    }
    case Kind::Fps:
        cfg_.video.fps = cycled(kFpsChoices, index_of(kFpsChoices, cfg_.video.fps), delta);
        break;
    case Kind::Background:
        cfg_.background_stream = !cfg_.background_stream;
        // Applies at once, even to a stream that is already running: the stream's supervisor only
        // looks at this when the app goes away.
        engine_.set_keep_running(cfg_.background_stream);
        toast_->show(cfg_.background_stream
                         ? "Stream keeps running when you leave. Stop: reopen ZeroCast, or run: zerocast --stop"
                         : "Leaving ZeroCast now stops the stream",
                     cfg_.background_stream ? widgets::ToastTone::Warning : widgets::ToastTone::Default, 4200);
        break;
    case Kind::Theme:
        vm_.toggle_dark_mode();
        break;
    }
    save_config();
    if (streaming() && item.kind != Kind::Theme && item.kind != Kind::Background)
        toast_->show("Applies the next time you go live", widgets::ToastTone::Default, 2200);
    refresh();
}

void ZeroCastUi::toggle_stream()
{
    const auto state = engine_.state();
    if (state == stream::EngineState::Live || state == stream::EngineState::Starting) {
        engine_.stop();
    } else if (state == stream::EngineState::Idle || state == stream::EngineState::Failed) {
        if (!engine_.start(cfg_))
            toast_->show(engine_.error(), widgets::ToastTone::Error, 3500);
    }
    refresh();
}

void ZeroCastUi::start_remote()
{
    auto apply = [this](const stream::Config& next) {
        cfg_ = next;
        engine_.set_keep_running(cfg_.background_stream);
        save_config();
        toast_->show("Settings saved from the web page", widgets::ToastTone::Success, 3000);
    };
    if (!web_.start(cfg_, apply, kWebPort, kWebTtlSec)) {
        toast_->show("Remote setup failed: " + web_.error(), widgets::ToastTone::Error, 3500);
        return;
    }
    mode_ = Mode::Remote;
    refresh();
}

void ZeroCastUi::stop_remote()
{
    web_.stop();
    mode_ = Mode::Browse;
    toast_->show("Remote setup closed", widgets::ToastTone::Default, 1800);
    refresh();
}

void ZeroCastUi::begin_edit(EditField field)
{
    edit_field_ = field;
    edit_reveal_ = false;
    // Never pre-fill a stream key: it is a secret and would appear in the edit line.
    edit_buf_ = field == EditField::Url ? cfg_.destinations[edit_dest_].url : std::string();
    mode_ = Mode::EditText;
    refresh();
}

void ZeroCastUi::commit_edit()
{
    auto& d = cfg_.destinations[edit_dest_];
    if (edit_field_ == EditField::Url) {
        if (edit_buf_.rfind("rtmp://", 0) != 0 && edit_buf_.rfind("rtmps://", 0) != 0) {
            toast_->show("URL must start with rtmp:// or rtmps://", widgets::ToastTone::Error, 3000);
            return;  // stay in the editor
        }
        d.url = edit_buf_;
    } else if (!edit_buf_.empty()) {
        d.key = edit_buf_;  // empty = keep the existing key
    }
    save_config();
    mode_ = Mode::Browse;
    toast_->show(d.name + (edit_field_ == EditField::Key ? ": key saved" : ": URL saved"),
                 widgets::ToastTone::Success, 1800);
    edit_buf_.clear();
    refresh();
}

void ZeroCastUi::take_screenshot()
{
    const auto result = platform::screenshot::capture_active_screen();
    if (result.success)
        toast_->show_highlighted("Saved to ", result.path, "", widgets::ToastTone::Success, 5200, true);
    else
        toast_->show("Screenshot failed: " + result.error, widgets::ToastTone::Error, 3200);
}

void ZeroCastUi::quit_requested()
{
    const auto state = engine_.state();
    const bool running = state == stream::EngineState::Live || state == stream::EngineState::Starting;
    if (running && cfg_.background_stream) {
        // "Keep streaming when closed": leave the stream running. The supervisor also survives
        // if the launcher kills us instead of this clean exit.
        engine_.detach();
    } else if (streaming()) {
        toast_->show("Stop the stream first (Enter), then hold Esc", widgets::ToastTone::Warning, 3200);
        return;
    }
    web_.stop();
    vm_.request_quit();
}

// ---------------------------------------------------------------------------------------------
// Model -> rows
// ---------------------------------------------------------------------------------------------

std::vector<ZeroCastUi::Item> ZeroCastUi::items(Page page) const
{
    std::vector<Item> out;
    switch (page) {
    case Page::Live: break;
    case Page::Sources:
        out.push_back({Kind::Video});
        if (cfg_.sources.video == stream::VideoSource::Screen) {
            out.push_back({Kind::Screen});
            if (cfg_.sources.screen == stream::ScreenTarget::Lcd)
                out.push_back({Kind::LcdNative});
        } else {
            if (cfg_.sources.video == stream::VideoSource::Usb)
            {
                out.push_back({Kind::UsbInfo});
                const CameraFormats& caps = camera_formats();
                if (!caps.known || (caps.mjpeg && caps.raw))  // a camera with one format has nothing to choose
                    out.push_back({Kind::UsbFormat});
            }
            out.push_back({Kind::Flip});
        }
        out.push_back({Kind::Mic});
        if (cfg_.sources.mic)
            out.push_back({Kind::MicInput});
        out.push_back({Kind::System});
        break;
    case Page::Destinations:
        for (size_t i = 0; i < cfg_.destinations.size(); ++i)
            out.push_back({Kind::Dest, static_cast<int>(i)});
        out.push_back({Kind::Remote});
        break;
    case Page::Settings:
        out.push_back({Kind::Bitrate});
        out.push_back({Kind::Resolution});
        out.push_back({Kind::Fps});
        out.push_back({Kind::Background});
        out.push_back({Kind::Theme});
        break;
    }
    return out;
}

ZeroCastUi::Row ZeroCastUi::describe(const Item& item) const
{
    const auto on_off = [](bool on) { return std::string(on ? "On" : "Off"); };
    const auto tone = [](bool on) { return on ? Tone::Good : Tone::Dim; };
    Row r;
    switch (item.kind) {
    case Kind::Video:
        r.left = "Video source";
        r.right = cfg_.sources.video == stream::VideoSource::Camera ? "Camera"
                  : cfg_.sources.video == stream::VideoSource::Usb  ? "USB camera"
                                                                   : "Screen";
        break;
    case Kind::UsbInfo: {
        const auto cams = stream::list_usb_cameras();
        r.left = "USB camera";
        const stream::UsbCamera* shown = cams.empty() ? nullptr : &cams.front();
        const bool chosen = cfg_.sources.usb_device != "auto";
        if (chosen) {
            shown = nullptr;
            for (const auto& cam : cams)
                if (cam.device == cfg_.sources.usb_device)
                    shown = &cam;
        }
        if (!shown) {
            r.right = chosen ? "missing, using first" : "not found";
            r.tone = Tone::Warn;
        } else {
            std::vector<std::string> labels;
            size_t index = 0;
            for (size_t i = 0; i < cams.size(); ++i) {
                labels.push_back(cams[i].name.empty() ? cams[i].device : tidy_name(cams[i].name));
                if (&cams[i] == shown)
                    index = i;
            }
            const std::string suffix = (cams.size() > 1 && !chosen) ? " (auto)" : "";
            std::string label = numbered(labels, index, 22 - suffix.size()) + suffix;
            r.right = label;
            r.tone = Tone::Good;
        }
        break;
    }
    case Kind::UsbFormat: {
        const CameraFormats& caps = camera_formats();
        // A saved choice this camera cannot do is shown (and used) as Auto.
        const bool unsupported = caps.known && ((cfg_.sources.usb_format == "mjpeg" && !caps.mjpeg) ||
                                                (cfg_.sources.usb_format == "raw" && !caps.raw));
        const std::string f = unsupported ? "auto" : cfg_.sources.usb_format;
        r.left = "Camera format";
        r.right = f == "mjpeg" ? "MJPEG (compressed)" : f == "auto" ? "Auto (best fit)" : f == "raw" ? "Raw (uncompressed)" : f;
        break;
    }
    case Kind::MicInput:
        r.left = "Mic input";
        r.right = mic_label(cfg_.sources.mic_device);
        break;
    case Kind::Screen:
        r.left = "Screen";
        r.right = cfg_.sources.screen == stream::ScreenTarget::Lcd ? "Built-in LCD" : "HDMI desktop";
        break;
    case Kind::Mic:
        r.left = "Microphone";
        r.right = on_off(cfg_.sources.mic);
        r.tone = tone(cfg_.sources.mic);
        break;
    case Kind::System:
        r.left = "System audio";
        r.right = on_off(cfg_.sources.system_audio);
        r.tone = tone(cfg_.sources.system_audio);
        break;
    case Kind::LcdNative:
        r.left = "LCD size";
        r.right = cfg_.sources.lcd_native ? "Native 320x170"
                                          : "Scaled " + std::to_string(cfg_.video.height) + "p";
        break;
    case Kind::Flip: {
        const bool usb = cfg_.sources.video == stream::VideoSource::Usb;
        const bool flipped = usb ? cfg_.sources.usb_rotate_180 : cfg_.sources.camera_rotate_180;
        r.left = "Flip camera 180";
        r.right = on_off(flipped);
        r.tone = tone(flipped);
        break;
    }
    case Kind::Dest: {
        const auto& d = cfg_.destinations[item.index];
        r.left = d.name;
        r.right = std::string(d.enabled ? "ON" : "off") + (d.key.empty() ? "  no key" : "  key set");
        r.tone = d.enabled ? (d.key.empty() ? Tone::Warn : Tone::Good) : Tone::Dim;
        break;
    }
    case Kind::Remote:
        r.left = "Remote setup (phone/PC)";
        r.right = "Start >";
        r.tone = Tone::Accent;
        break;
    case Kind::Bitrate:
        r.left = "Video bitrate";
        r.right = std::to_string(cfg_.video.bitrate_kbps) + " kbps";
        break;
    case Kind::Resolution:
        r.left = "Resolution";
        r.right = std::to_string(cfg_.video.width) + "x" + std::to_string(cfg_.video.height);
        break;
    case Kind::Fps:
        r.left = "Frame rate";
        r.right = std::to_string(cfg_.video.fps) + " fps";
        break;
    case Kind::Background:
        r.left = "Keep streaming when closed";
        r.right = on_off(cfg_.background_stream);
        r.tone = cfg_.background_stream ? Tone::Warn : Tone::Dim;  // on = it keeps using the network
        break;
    case Kind::Theme:
        r.left = "Theme";
        r.right = vm_.is_dark_mode() ? "Dark" : "Light";
        break;
    }
    return r;
}

std::string ZeroCastUi::source_summary() const
{
    std::string video = "Camera";
    if (cfg_.sources.video == stream::VideoSource::Usb)
        video = "USB cam";
    else if (cfg_.sources.video == stream::VideoSource::Screen)
        video = cfg_.sources.screen == stream::ScreenTarget::Lcd ? "LCD" : "HDMI";
    std::string audio = "no audio";
    if (cfg_.sources.mic && cfg_.sources.system_audio)
        audio = "Mic + System";
    else if (cfg_.sources.mic)
        audio = "Mic";
    else if (cfg_.sources.system_audio)
        audio = "System audio";
    return video + " + " + audio;
}

std::string ZeroCastUi::destination_summary() const
{
    std::string out;
    for (const auto& d : cfg_.destinations) {
        if (!d.enabled)
            continue;
        out += (out.empty() ? "" : ", ") + d.name;
    }
    return out;
}

std::vector<ZeroCastUi::Row> ZeroCastUi::rows_live() const
{
    std::vector<Row> rows;
    const auto state = engine_.state();
    const auto& st = engine_.stats();

    Row status;
    status.size = 20;
    status.center = true;
    switch (state) {
    case stream::EngineState::Idle:
        status.left = "OFFLINE";
        status.tone = Tone::Dim;
        break;
    case stream::EngineState::Starting:
        status.left = "STARTING...";
        status.tone = Tone::Warn;
        break;
    case stream::EngineState::Live:
        status.left = "LIVE  " + clock_text(st.uptime_sec);
        status.tone = Tone::Bad;  // red, like an on-air light
        break;
    case stream::EngineState::Stopping:
        status.left = "STOPPING...";
        status.tone = Tone::Warn;
        break;
    case stream::EngineState::Failed:
        status.left = "FAILED";
        status.tone = Tone::Bad;
        break;
    }
    rows.push_back(status);

    Row stats;
    stats.center = true;
    stats.tone = Tone::Dim;
    if (state == stream::EngineState::Live) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.0f fps   %.1f Mbps   dropped %ld", st.fps, st.bitrate_kbps / 1000.0,
                      st.dropped_frames);
        stats.left = buf;
    } else if (state == stream::EngineState::Failed) {
        // The row is one line wide: show the start of the message only.
        std::string msg = engine_.error().substr(0, engine_.error().find('\n'));
        if (msg.size() > 38)
            msg = msg.substr(0, 37) + "~";
        stats.left = msg;
        stats.tone = Tone::Bad;
    }
    rows.push_back(stats);

    Row src;
    src.left = "Source";
    src.right = source_summary();
    rows.push_back(src);

    Row dst;
    dst.left = "To";
    dst.right = destination_summary();
    if (dst.right.empty()) {
        dst.right = "none - see Destinations";
        dst.tone = Tone::Warn;
    }
    rows.push_back(dst);

    Row action;
    action.size = 14;
    action.center = true;
    action.tone = Tone::Accent;
    const bool running = state == stream::EngineState::Live || state == stream::EngineState::Starting;
    action.left = state == stream::EngineState::Stopping ? "" : (running ? "[Enter] STOP" : "[Enter] GO LIVE");
    rows.push_back(action);
    return rows;
}

std::vector<ZeroCastUi::Row> ZeroCastUi::rows_list(Page page) const
{
    const auto list = items(page);
    std::vector<Row> all;
    for (const auto& item : list)
        all.push_back(describe(item));
    const int sel = std::min(selection_[static_cast<int>(page)], std::max(0, static_cast<int>(all.size()) - 1));
    const int n = static_cast<int>(all.size());
    const int first = std::clamp(sel - (kMaxVisibleRows - 1), 0, std::max(0, n - kMaxVisibleRows));

    std::vector<Row> rows;
    for (int i = first; i < std::min(n, first + kMaxVisibleRows); ++i) {
        Row r = all[i];
        r.selected = (i == sel);
        rows.push_back(r);
    }
    return rows;
}

std::vector<ZeroCastUi::Row> ZeroCastUi::rows_edit() const
{
    const auto& d = cfg_.destinations[edit_dest_];
    std::vector<Row> rows;
    Row title;
    title.size = 14;
    title.center = true;
    title.tone = Tone::Accent;
    title.left = d.name + (edit_field_ == EditField::Key ? " - stream key" : " - server URL");
    rows.push_back(title);

    // Masked unless revealed: show only the tail so a long key fits and a shoulder-surfer
    // does not get the whole thing. The last character stays visible for typing feedback.
    std::string shown;
    if (edit_reveal_ || edit_field_ == EditField::Url) {
        shown = edit_buf_;
    } else {
        shown.assign(edit_buf_.size() > 0 ? edit_buf_.size() - 1 : 0, '*');
        if (!edit_buf_.empty())
            shown += edit_buf_.back();
    }
    constexpr size_t kShown = 34;
    if (shown.size() > kShown)
        shown = "..." + shown.substr(shown.size() - (kShown - 3));
    Row field;
    field.size = 14;
    field.center = true;
    field.left = shown + "_";
    rows.push_back(field);

    Row len;
    len.center = true;
    len.tone = Tone::Dim;
    len.left = std::to_string(edit_buf_.size()) + " characters";
    if (edit_field_ == EditField::Key && d.key.empty() == false)
        len.left += "   (blank + Enter keeps the saved key)";
    rows.push_back(len);

    if (edit_field_ == EditField::Url) {
        Row hint;
        hint.center = true;
        hint.tone = Tone::Dim;
        hint.left = "e.g. rtmp://host:1935/live";
        rows.push_back(hint);
    }
    return rows;
}

std::vector<ZeroCastUi::Row> ZeroCastUi::rows_remote() const
{
    std::vector<Row> rows;
    Row title;
    title.size = 14;
    title.center = true;
    title.tone = Tone::Accent;
    title.left = "Remote setup";
    rows.push_back(title);

    // Browsers always ask for a user name as well as the password, so show both. The server
    // accepts any user name; "zerocast" is just what to type.
    Row url;
    url.size = 14;
    url.center = true;
    url.left = web_.running() ? web_.url() : "";
    rows.push_back(url);

    Row user;
    user.center = true;
    user.tone = Tone::Dim;
    user.left = "User: zerocast   (any name works)";
    rows.push_back(user);

    Row pw;
    pw.size = 20;
    pw.center = true;
    pw.tone = Tone::Good;
    pw.left = "Password " + web_.password();
    rows.push_back(pw);

    Row left;
    left.center = true;
    left.tone = Tone::Warn;
    left.left = "Closes in " + minutes_text(web_.seconds_left()) + "   same Wi-Fi, not encrypted";
    rows.push_back(left);
    return rows;
}

std::string ZeroCastUi::footer_text() const
{
    if (mode_ == Mode::EditText)
        return "Enter save   Esc cancel   Tab show/hide";
    if (mode_ == Mode::Remote)
        return "Esc stops remote setup";
    switch (page_) {
    case Page::Live:
        if (cfg_.background_stream && streaming())
            return "Enter stop  Hold Esc: exit, stream keeps running";
        return "Enter go live/stop  Tab page  Hold Esc exit";
    case Page::Destinations: return "Enter on/off  E key  U url  Tab page";
    default: return "Up/Dn select  Enter/L/R change  Esc back";
    }
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------

lv_color_t ZeroCastUi::tone_color(Tone tone) const
{
    const auto p = palette(vm_.is_dark_mode());
    switch (tone) {
    case Tone::Dim: return p.text_disabled;
    case Tone::Good: return p.success;
    case Tone::Warn: return p.warning;
    case Tone::Bad: return p.error;
    case Tone::Accent: return p.primary;
    case Tone::Normal: break;
    }
    return p.text;
}

const lv_font_t* ZeroCastUi::font_for(int size, bool bold) const
{
    const auto weight = bold ? app::StandardFontWeight::Bold : app::StandardFontWeight::Regular;
    const lv_font_t* f = assets_.load_standard_font(static_cast<uint32_t>(size), weight);
    if (f)
        return f;
    switch (size) {
    case 20: return &lv_font_montserrat_20;
    case 14: return &lv_font_montserrat_14;
    default: return &lv_font_montserrat_12;
    }
}

void ZeroCastUi::draw(const std::vector<Row>& rows, const std::string& footer)
{
    const bool dark = vm_.is_dark_mode();
    if (drawn_ && rows == last_rows_ && footer == last_footer_ && dark == last_dark_)
        return;
    drawn_ = true;
    last_rows_ = rows;
    last_footer_ = footer;
    last_dark_ = dark;

    lv_obj_clean(content_);
    lv_obj_clean(footer_);

    const auto p = palette(dark);
    // Selected-row text must contrast with the primary fill in both themes.
    const lv_color_t on_primary = dark ? lv_color_black() : lv_color_white();

    // Lay rows out top to bottom; big rows get more height. Vertically centre short lists.
    int total = 0;
    for (const auto& r : rows)
        total += r.size >= 20 ? 28 : (r.size >= 14 ? 22 : 20);
    int y = std::max(2, (kContentHeight - total) / 2);

    for (const auto& r : rows) {
        const int h = r.size >= 20 ? 28 : (r.size >= 14 ? 22 : 20);
        auto* row = lv_obj_create(content_);
        lv_obj_remove_style_all(row);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, kScreenWidth - 8, h);
        lv_obj_set_pos(row, 4, y);
        y += h;
        if (r.selected) {
            lv_obj_set_style_bg_color(row, p.primary, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(row, 4, 0);
        }
        const lv_color_t color = r.selected ? on_primary : tone_color(r.tone);
        const lv_font_t* font = font_for(r.size, r.size >= 14);

        if (r.center) {
            auto* label = lv_label_create(row);
            lv_obj_set_width(label, LV_PCT(100));
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(label, font, 0);
            lv_obj_set_style_text_color(label, color, 0);
            lv_label_set_text(label, r.left.c_str());
            lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        } else {
            auto* left = lv_label_create(row);
            lv_obj_set_style_text_font(left, font, 0);
            lv_obj_set_style_text_color(left, r.selected ? on_primary : p.text, 0);
            lv_label_set_text(left, r.left.c_str());
            lv_obj_align(left, LV_ALIGN_LEFT_MID, 6, 0);

            auto* right = lv_label_create(row);
            lv_label_set_long_mode(right, LV_LABEL_LONG_DOT);
            lv_obj_set_style_max_width(right, 170, 0);
            lv_obj_set_style_text_align(right, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_style_text_font(right, font, 0);
            lv_obj_set_style_text_color(right, color, 0);
            lv_label_set_text(right, r.right.c_str());
            lv_obj_align(right, LV_ALIGN_RIGHT_MID, -6, 0);
        }
    }

    auto* hint = lv_label_create(footer_);
    lv_obj_set_width(hint, kScreenWidth - 8);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(hint, font_for(12, false), 0);
    lv_obj_set_style_text_color(hint, p.text_disabled, 0);
    lv_label_set_text(hint, footer.c_str());
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
}

void ZeroCastUi::refresh()
{
    std::vector<Row> rows;
    if (mode_ == Mode::EditText)
        rows = rows_edit();
    else if (mode_ == Mode::Remote)
        rows = rows_remote();
    else if (page_ == Page::Live)
        rows = rows_live();
    else
        rows = rows_list(page_);
    draw(rows, footer_text());
}

void ZeroCastUi::tick_cb(lv_timer_t* timer)
{
    static_cast<ZeroCastUi*>(lv_timer_get_user_data(timer))->tick();
}

void ZeroCastUi::tick()
{
    engine_.poll();

    if (web_.running()) {
        web_.poll();
        if (!web_.running() && mode_ == Mode::Remote) {
            const std::string why = web_.stop_reason();
            mode_ = Mode::Browse;
            if (why == "expired")
                toast_->show("Remote setup timed out", widgets::ToastTone::Default, 2200);
            else if (why == "saved")
                toast_->show("Remote setup closed", widgets::ToastTone::Default, 2200);
            else if (why == "locked")
                toast_->show("Remote setup locked: too many wrong passwords", widgets::ToastTone::Warning, 3500);
        }
    }
    refresh();
}

}  // namespace view
