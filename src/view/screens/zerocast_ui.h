/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "asset_manager.h"
#include "base_viewmodel.h"
#include "stream_config.h"
#include "stream_engine.h"
#include "titlebar.h"
#include "toast.h"
#include "web_config.h"

#include "lvgl.h"

#include <memory>
#include <string>
#include <vector>

namespace view {

// The whole ZeroCast screen: Live / Sources / Destinations / Settings pages, the stream-key
// entry overlay and the opt-in remote (LAN web) setup overlay, on the 320x170 LCD.
// Everything is driven by one key handler; see handle_key() for the key map.
class ZeroCastUi {
public:
    ZeroCastUi(viewmodel::BaseViewModel& view_model,
               app::AssetManager& assets,
               stream::Config config,
               std::string config_path);
    ~ZeroCastUi();

    ZeroCastUi(const ZeroCastUi&) = delete;
    ZeroCastUi& operator=(const ZeroCastUi&) = delete;

    bool streaming() const;

private:
    enum class Page { Live, Sources, Destinations, Settings };
    enum class Mode { Browse, EditText, Remote };
    enum class Tone { Normal, Dim, Good, Warn, Bad, Accent };
    enum class Kind { Video, UsbInfo, UsbFormat, Screen, Mic, MicInput, System, LcdNative, Flip, Dest, Remote, Bitrate, Resolution, Fps, Background, Theme };
    enum class EditField { Key, Url };

    struct Item {
        Kind kind;
        int index = 0;
    };

    struct Row {
        std::string left;
        std::string right;
        Tone tone = Tone::Normal;
        bool selected = false;
        int size = 12;      // font size: 12, 14 or 20
        bool center = false;  // single centred label (right is ignored)

        bool operator==(const Row& o) const {
            return left == o.left && right == o.right && tone == o.tone && selected == o.selected &&
                   size == o.size && center == o.center;
        }
    };

    // Input
    bool handle_key(uint32_t key, bool long_pressed);
    void handle_release(uint32_t key);
    bool handle_edit_key(uint32_t key);
    bool handle_browse_key(uint32_t key);
    static bool key_cb(uint32_t key, const char* name, bool long_pressed, void* user_data);
    static void release_cb(uint32_t key, const char* name, void* user_data);

    // Actions
    void set_page(Page page);
    void move_selection(int delta);
    void change(const Item& item, int delta);
    void toggle_stream();
    void start_remote();
    void stop_remote();
    void begin_edit(EditField field);
    void commit_edit();
    void save_config();
    void take_screenshot();
    void quit_requested();

    // Model -> rows
    std::vector<Item> items(Page page) const;
    Row describe(const Item& item) const;
    std::vector<Row> rows_live() const;
    std::vector<Row> rows_list(Page page) const;
    std::vector<Row> rows_edit() const;
    std::vector<Row> rows_remote() const;
    std::string footer_text() const;
    std::string source_summary() const;
    std::string destination_summary() const;

    // Drawing
    void refresh();
    void draw(const std::vector<Row>& rows, const std::string& footer);
    lv_color_t tone_color(Tone tone) const;
    const lv_font_t* font_for(int size, bool bold) const;
    void tick();
    static void tick_cb(lv_timer_t* timer);

    viewmodel::BaseViewModel& vm_;
    app::AssetManager& assets_;
    stream::Config cfg_;
    std::string cfg_path_;
    stream::StreamEngine engine_;
    stream::WebConfigServer web_;

    Page page_ = Page::Live;
    Mode mode_ = Mode::Browse;
    int selection_[4] = {0, 0, 0, 0};

    // What the selected USB camera can deliver, probed once per camera (probing runs v4l2-ctl).
    struct CameraFormats {
        bool known = false;  // false: could not be probed, offer every choice
        bool mjpeg = false;
        bool raw = false;
    };
    const CameraFormats& camera_formats() const;
    mutable std::string probed_device_;
    mutable CameraFormats probed_formats_;
    EditField edit_field_ = EditField::Key;
    int edit_dest_ = 0;
    std::string edit_buf_;
    bool edit_reveal_ = false;
    bool esc_down_ = false;
    bool esc_long_done_ = false;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* footer_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    std::unique_ptr<widgets::TitleBar> title_bar_;
    std::unique_ptr<widgets::Toast> toast_;
    std::vector<Row> last_rows_;
    std::string last_footer_;
    bool last_dark_ = false;
    bool drawn_ = false;
};

}  // namespace view
