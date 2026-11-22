#pragma once

#include <string>

namespace sb3 {
namespace web_pages {

extern const char* const kCss;
extern const char* const kCommonJs;

std::string page_body(const std::string& page);
std::string page_script(const std::string& page);

}  // namespace web_pages
}  // namespace sb3
