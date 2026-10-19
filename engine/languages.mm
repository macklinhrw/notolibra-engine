// Copyright 2026 Notolibra. All rights reserved.

#include "notolibra/engine/languages.h"

#import <Foundation/Foundation.h>

#include <vector>

#include <algorithm>
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/strings/sys_string_conversions.h"
#include "net/http/http_util.h"

namespace notolibra {

namespace {

// "zh-Hans-CN" -> "zh-CN", "en-US" -> "en-US", "fr" -> "fr": a language and
// region, as Chrome's language settings list them (script subtags dropped).
std::string ChromeLanguageCode(const std::string& tag) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= tag.size()) {
    size_t end = tag.find_first_of("-_", start);
    if (end == std::string::npos) {
      end = tag.size();
    }
    parts.push_back(tag.substr(start, end - start));
    start = end + 1;
  }
  if (parts.empty() || parts[0].empty()) {
    return std::string();
  }
  std::string code = base::ToLowerASCII(parts[0]);
  for (size_t i = 1; i < parts.size(); ++i) {
    // A region: two letters or three digits.
    if ((parts[i].size() == 2 && base::IsAsciiAlpha(parts[i][0])) ||
        (parts[i].size() == 3 && base::IsAsciiDigit(parts[i][0]))) {
      code += "-" + base::ToUpperASCII(parts[i]);
      break;
    }
  }
  return code;
}

std::string ComputeAcceptLanguages() {
  std::vector<std::string> codes;
  @autoreleasepool {
    for (NSString* tag in [NSLocale preferredLanguages]) {
      std::string code = ChromeLanguageCode(base::SysNSStringToUTF8(tag));
      if (!code.empty() && std::ranges::find(codes, code) == codes.end()) {
        codes.push_back(code);
      }
    }
  }
  if (codes.empty()) {
    codes.push_back("en-US");
  }
  return net::HttpUtil::ExpandLanguageList(base::JoinString(codes, ","));
}

}  // namespace

const std::string& AcceptLanguages() {
  static const base::NoDestructor<std::string> languages(
      ComputeAcceptLanguages());
  return *languages;
}

std::string AcceptLanguageHeader() {
  return net::HttpUtil::GenerateAcceptLanguageHeader(AcceptLanguages());
}

}  // namespace notolibra
