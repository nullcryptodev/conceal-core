#pragma once

#include <string>

namespace PathHelpers
{
  inline std::string appendPath(const std::string &path, const std::string &fileName)
  {
    std::string result = path;
    if (!result.empty() && result.back() != '/')
      result += '/';
    result += fileName;
    return result;
  }
}