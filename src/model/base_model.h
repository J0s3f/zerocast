/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

namespace model {

// Shell state shared by the title bar and theme: the page title and light/dark mode.
// The streaming state itself lives in stream::Config / stream::StreamEngine.
class BaseModel {
public:
    const std::string& title() const;
    void set_title(const std::string& title);

    bool dark_mode() const;
    void set_dark_mode(bool enabled);
    void toggle_dark_mode();

private:
    std::string title_ = "ZeroCast";
    bool dark_mode_ = true;  // dark by default: easier on the eyes and on OLED-like panels
};

} // namespace model
