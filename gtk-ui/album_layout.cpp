// Telegram Desktop's Ui::LayoutMediaGroup without Qt. GPLv3, as the rest of this program.
#include "album_layout.hpp"

#include <numeric>
#include <string>

namespace Contrail {
namespace {

int Round(double value) {
  return int(std::lround(value));
}

std::vector<Rect> LayoutMany(const std::vector<double> &source, double averageRatio, int maxWidth, int minWidth,
                             int spacing) {
  constexpr auto kMaxRatio = 2.75;
  constexpr auto kMinRatio = 0.6667;
  auto ratios = source;
  for (auto &ratio : ratios) {
    ratio = averageRatio > 1.1 ? std::clamp(ratio, 1., kMaxRatio) : std::clamp(ratio, kMinRatio, 1.);
  }
  const auto count = int(ratios.size());
  const auto maxHeight = maxWidth * 4 / 3;  // In this case the official apps let an album be taller.

  struct Attempt {
    std::vector<int> lineCounts;
    std::vector<double> heights;
  };
  auto attempts = std::vector<Attempt>();
  const auto lineHeight = [&](int offset, int line) {
    const auto sum = std::accumulate(ratios.begin() + offset, ratios.begin() + offset + line, 0.);
    return (maxWidth - (line - 1) * spacing) / sum;
  };
  const auto push = [&](std::vector<int> lineCounts) {
    auto heights = std::vector<double>();
    auto offset = 0;
    for (const auto line : lineCounts) {
      heights.push_back(lineHeight(offset, line));
      offset += line;
    }
    attempts.push_back({std::move(lineCounts), std::move(heights)});
  };
  for (auto first = 1; first != count; ++first) {
    const auto second = count - first;
    if (first <= 3 && second <= 3) push({first, second});
  }
  for (auto first = 1; first < count - 1; ++first) {
    for (auto second = 1; second != count - first; ++second) {
      const auto third = count - first - second;
      if (first <= 3 && second <= (averageRatio < 0.85 ? 4 : 3) && third <= 3) push({first, second, third});
    }
  }
  for (auto first = 1; first < count - 1; ++first) {
    for (auto second = 1; second < count - first; ++second) {
      for (auto third = 1; third < count - first - second; ++third) {
        const auto fourth = count - first - second - third;
        if (first <= 3 && second <= 3 && third <= 3 && fourth <= 3) push({first, second, third, fourth});
      }
    }
  }
  if (attempts.empty()) return {};  // More pictures than four lines of three: Telegram's albums have ten at most.

  const Attempt *best = nullptr;
  auto bestDiff = 0.;
  for (const auto &attempt : attempts) {
    const auto lines = int(attempt.lineCounts.size());
    const auto total = std::accumulate(attempt.heights.begin(), attempt.heights.end(), 0.) + spacing * (lines - 1);
    const auto shortest = *std::min_element(attempt.heights.begin(), attempt.heights.end());
    const auto tooShort = shortest < minWidth ? 1.5 : 1.;
    auto shrinking = 1.;
    for (auto line = 1; line != lines; ++line) {
      if (attempt.lineCounts[line - 1] > attempt.lineCounts[line]) shrinking = 1.5;
    }
    const auto diff = std::abs(total - maxHeight) * tooShort * shrinking;
    if (!best || diff < bestDiff) {
      best = &attempt;
      bestDiff = diff;
    }
  }

  auto result = std::vector<Rect>(count);
  auto index = 0;
  auto y = 0;
  for (auto row = 0; row != int(best->lineCounts.size()); ++row) {
    const auto columns = best->lineCounts[row];
    const auto line = best->heights[row];
    const auto height = Round(line);
    auto x = 0;
    for (auto column = 0; column != columns; ++column) {
      const auto width = column == columns - 1 ? maxWidth - x : Round(ratios[index] * line);
      result[index++] = Rect{x, y, width, height};
      x += width + spacing;
    }
    y += height + spacing;
  }
  return result;
}

}  // namespace

std::vector<Rect> LayoutAlbum(const std::vector<Size> &sizes, int maxWidth, int minWidth, int spacing) {
  const auto count = int(sizes.size());
  if (!count) return {};
  auto ratios = std::vector<double>();
  auto proportions = std::string();
  for (const auto &size : sizes) {
    const auto ratio = double(size.width) / size.height;
    ratios.push_back(ratio);
    proportions += ratio > 1.2 ? 'w' : ratio < 0.8 ? 'n' : 'q';
  }
  const auto average = std::accumulate(ratios.begin(), ratios.end(), 1.) / count;
  const auto wide = std::any_of(ratios.begin(), ratios.end(), [](double ratio) { return ratio > 2; });
  if (count == 1) return {{0, 0, maxWidth, sizes[0].height * maxWidth / sizes[0].width}};
  if (count >= 5 || wide) return LayoutMany(ratios, average, maxWidth, minWidth, spacing);

  // The official apps use a square at first; only the case above may be taller.
  const auto maxHeight = maxWidth;
  const auto maxRatio = double(maxWidth) / maxHeight;
  const auto &r = ratios;
  if (count == 2) {
    if (proportions == "ww" && average > 1.4 * maxRatio && r[1] - r[0] < 0.2) {
      const auto height = Round(std::min({maxWidth / r[0], maxWidth / r[1], (maxHeight - spacing) / 2.}));
      return {{0, 0, maxWidth, height}, {0, height + spacing, maxWidth, height}};
    }
    if (proportions == "ww" || proportions == "qq") {
      const auto width = (maxWidth - spacing) / 2;
      const auto height = Round(std::min({width / r[0], width / r[1], maxHeight * 1.}));
      return {{0, 0, width, height}, {width + spacing, 0, width, height}};
    }
    const auto minimal = Round(minWidth * 1.5);
    const auto second = std::min(Round(std::max(0.4 * (maxWidth - spacing),
                                                (maxWidth - spacing) / r[0] / (1. / r[0] + 1. / r[1]))),
                                 maxWidth - spacing - minimal);
    const auto first = maxWidth - second - spacing;
    const auto height = std::min(maxHeight, Round(std::min(first / r[0], second / r[1])));
    return {{0, 0, first, height}, {first + spacing, 0, second, height}};
  }
  if (count == 3) {
    if (proportions[0] == 'n') {
      const auto firstHeight = maxHeight;
      const auto third = Round(std::min((maxHeight - spacing) / 2., r[1] * (maxWidth - spacing) / (r[2] + r[1])));
      const auto second = firstHeight - third - spacing;
      const auto right = std::max(minWidth, Round(std::min({(maxWidth - spacing) / 2., third * r[2], second * r[1]})));
      const auto left = std::min(Round(firstHeight * r[0]), maxWidth - spacing - right);
      return {{0, 0, left, firstHeight}, {left + spacing, 0, right, second},
              {left + spacing, second + spacing, right, third}};
    }
    const auto firstHeight = Round(std::min(maxWidth / r[0], (maxHeight - spacing) * 0.66));
    const auto second = (maxWidth - spacing) / 2;
    const auto height = std::min(maxHeight - firstHeight - spacing, Round(std::min(second / r[1], second / r[2])));
    const auto third = maxWidth - second - spacing;
    return {{0, 0, maxWidth, firstHeight}, {0, firstHeight + spacing, second, height},
            {second + spacing, firstHeight + spacing, third, height}};
  }
  if (proportions[0] == 'w') {
    const auto h0 = Round(std::min(maxWidth / r[0], (maxHeight - spacing) * 0.66));
    const auto h = Round((maxWidth - 2 * spacing) / (r[1] + r[2] + r[3]));
    const auto w0 = std::max(minWidth, Round(std::min((maxWidth - 2 * spacing) * 0.4, h * r[1])));
    const auto w2 = Round(std::max({minWidth * 1., (maxWidth - 2 * spacing) * 0.33, h * r[3]}));
    const auto w1 = maxWidth - w0 - w2 - 2 * spacing;
    const auto h1 = std::min(maxHeight - h0 - spacing, h);
    return {{0, 0, maxWidth, h0}, {0, h0 + spacing, w0, h1}, {w0 + spacing, h0 + spacing, w1, h1},
            {w0 + spacing + w1 + spacing, h0 + spacing, w2, h1}};
  }
  const auto h = maxHeight;
  const auto w0 = Round(std::min(h * r[0], (maxWidth - spacing) * 0.6));
  const auto w = Round((maxHeight - 2 * spacing) / (1. / r[1] + 1. / r[2] + 1. / r[3]));
  const auto h0 = Round(w / r[1]);
  const auto h1 = Round(w / r[2]);
  const auto h2 = h - h0 - h1 - 2 * spacing;
  const auto w1 = std::max(minWidth, std::min(maxWidth - w0 - spacing, w));
  return {{0, 0, w0, h}, {w0 + spacing, 0, w1, h0}, {w0 + spacing, h0 + spacing, w1, h1},
          {w0 + spacing, h0 + h1 + 2 * spacing, w1, h2}};
}

}  // namespace Contrail
