#pragma once

#include <string>

namespace lvgl_password_prompt {

// Returns true when password was accepted; false when cancelled.
bool prompt(const std::string& title, const std::string& subtitle, std::string& outPassword);

}  // namespace lvgl_password_prompt
