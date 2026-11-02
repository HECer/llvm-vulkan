// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "lv/compute.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace lv::cpu {
void validate_graph(const Graph &g) {
  if (g.offsets.empty()) {
    if (!g.edges.empty() || !g.components.empty())
      throw std::invalid_argument("empty graph has edges or components");
    return;
  }
  if (g.nodes() > UINT32_MAX || g.edges.size() > UINT32_MAX)
    throw std::length_error("graph exceeds 32-bit indices");
  if (g.offsets.front() != 0 || g.offsets.back() != g.edges.size())
    throw std::invalid_argument("invalid CSR boundary offsets");
  for (size_t i = 1; i < g.offsets.size(); ++i)
    if (g.offsets[i] < g.offsets[i - 1])
      throw std::invalid_argument("nonmonotonic CSR offsets");
  for (auto v : g.edges)
    if (v >= g.nodes())
      throw std::invalid_argument("CSR edge is out of range");
  if (!g.components.empty()) {
    if (g.components.front() != 0 || g.components.back() != g.nodes())
      throw std::invalid_argument("invalid graph component boundaries");
    for (size_t c = 1; c < g.components.size(); ++c) {
      uint32_t first = g.components[c - 1], end = g.components[c];
      if (first >= end || end > g.nodes())
        throw std::invalid_argument(
            "graph components must be nonempty and ordered");
      for (uint32_t v = first; v < end; ++v)
        for (uint32_t e = g.offsets[v]; e < g.offsets[v + 1]; ++e)
          if (g.edges[e] < first || g.edges[e] >= end)
            throw std::invalid_argument(
                "edge crosses independent graph components");
    }
  }
}
static int64_t signed_value(uint32_t x, uint32_t w) {
  return (x & (uint32_t(1) << (w - 1))) ? int64_t(x) - (int64_t(1) << w)
                                        : int64_t(x);
}
std::vector<FoldResult> fold(const std::vector<FoldRequest> &in) {
  std::vector<FoldResult> out;
  out.reserve(in.size());
  for (auto t : in) {
    FoldResult r{0, 0};
    if (t.width < 1 || t.width > 32 || t.op > uint32_t(FoldOp::Sge)) {
      out.push_back(r);
      continue;
    }
    uint32_t mask = t.width == 32 ? UINT32_MAX : (uint32_t(1) << t.width) - 1;
    uint32_t a = t.lhs & mask, b = t.rhs & mask, v = 0;
    int64_t sa = signed_value(a, t.width), sb = signed_value(b, t.width);
    bool ok = true, cmp = false;
    switch (FoldOp(t.op)) {
    case FoldOp::Add:
      v = a + b;
      break;
    case FoldOp::Sub:
      v = a - b;
      break;
    case FoldOp::Mul:
      v = uint32_t(uint64_t(a) * b);
      break;
    case FoldOp::And:
      v = a & b;
      break;
    case FoldOp::Or:
      v = a | b;
      break;
    case FoldOp::Xor:
      v = a ^ b;
      break;
    case FoldOp::Shl:
      if (b >= t.width)
        ok = false;
      else
        v = a << b;
      break;
    case FoldOp::LShr:
      if (b >= t.width)
        ok = false;
      else
        v = a >> b;
      break;
    case FoldOp::AShr:
      if (b >= t.width)
        ok = false;
      else {
        v = a >> b;
        if (sa < 0 && b)
          v |= mask ^ (mask >> b);
      }
      break;
    case FoldOp::UDiv:
      if (!b)
        ok = false;
      else
        v = a / b;
      break;
    case FoldOp::SDiv:
      if (!sb || (sa == -(int64_t(1) << (t.width - 1)) && sb == -1))
        ok = false;
      else
        v = uint32_t(sa / sb);
      break;
    case FoldOp::URem:
      if (!b)
        ok = false;
      else
        v = a % b;
      break;
    case FoldOp::SRem:
      if (!sb || (sa == -(int64_t(1) << (t.width - 1)) && sb == -1))
        ok = false;
      else
        v = uint32_t(sa % sb);
      break;
    case FoldOp::Eq:
      v = a == b;
      cmp = true;
      break;
    case FoldOp::Ne:
      v = a != b;
      cmp = true;
      break;
    case FoldOp::Ult:
      v = a < b;
      cmp = true;
      break;
    case FoldOp::Ule:
      v = a <= b;
      cmp = true;
      break;
    case FoldOp::Ugt:
      v = a > b;
      cmp = true;
      break;
    case FoldOp::Uge:
      v = a >= b;
      cmp = true;
      break;
    case FoldOp::Slt:
      v = sa < sb;
      cmp = true;
      break;
    case FoldOp::Sle:
      v = sa <= sb;
      cmp = true;
      break;
    case FoldOp::Sgt:
      v = sa > sb;
      cmp = true;
      break;
    case FoldOp::Sge:
      v = sa >= sb;
      cmp = true;
      break;
    }
    if (ok)
      r = {cmp ? v : v & mask, 1};
    out.push_back(r);
  }
  return out;
}
std::vector<Fold64Result> fold64(const std::vector<Fold64Request> &in) {
  std::vector<Fold64Result> out;
  out.reserve(in.size());
  for (auto t : in) {
    Fold64Result r{0, 0, 0};
    if (t.width < 1 || t.width > 64 || t.op > uint32_t(FoldOp::Sge)) {
      out.push_back(r);
      continue;
    }
    const uint64_t mask =
        t.width == 64 ? UINT64_MAX : (uint64_t(1) << t.width) - 1;
    const uint64_t sign = uint64_t(1) << (t.width - 1);
    const uint64_t a = t.lhs & mask, b = t.rhs & mask;
    const bool an = (a & sign) != 0, bn = (b & sign) != 0;
    const uint64_t ma = an ? (uint64_t(0) - a) & mask : a,
                   mb = bn ? (uint64_t(0) - b) & mask : b;
    const bool slt = an != bn ? an : a < b;
    bool ok = true;
    uint64_t v = 0;
    switch (FoldOp(t.op)) {
    case FoldOp::Add:
      v = a + b;
      break;
    case FoldOp::Sub:
      v = a - b;
      break;
    case FoldOp::Mul:
      v = a * b;
      break;
    case FoldOp::And:
      v = a & b;
      break;
    case FoldOp::Or:
      v = a | b;
      break;
    case FoldOp::Xor:
      v = a ^ b;
      break;
    case FoldOp::Shl:
      if (b >= t.width)
        ok = false;
      else
        v = a << b;
      break;
    case FoldOp::LShr:
      if (b >= t.width)
        ok = false;
      else
        v = a >> b;
      break;
    case FoldOp::AShr:
      if (b >= t.width)
        ok = false;
      else {
        v = a >> b;
        if (an && b)
          v |= mask ^ (mask >> b);
      }
      break;
    case FoldOp::UDiv:
      if (!b)
        ok = false;
      else
        v = a / b;
      break;
    case FoldOp::URem:
      if (!b)
        ok = false;
      else
        v = a % b;
      break;
    case FoldOp::SDiv:
      if (!b || (a == sign && b == mask))
        ok = false;
      else {
        v = ma / mb;
        if (an != bn)
          v = uint64_t(0) - v;
      }
      break;
    case FoldOp::SRem:
      if (!b || (a == sign && b == mask))
        ok = false;
      else {
        v = ma % mb;
        if (an)
          v = uint64_t(0) - v;
      }
      break;
    case FoldOp::Eq:
      v = a == b;
      break;
    case FoldOp::Ne:
      v = a != b;
      break;
    case FoldOp::Ult:
      v = a < b;
      break;
    case FoldOp::Ule:
      v = a <= b;
      break;
    case FoldOp::Ugt:
      v = a > b;
      break;
    case FoldOp::Uge:
      v = a >= b;
      break;
    case FoldOp::Slt:
      v = slt;
      break;
    case FoldOp::Sle:
      v = slt || a == b;
      break;
    case FoldOp::Sgt:
      v = !slt && a != b;
      break;
    case FoldOp::Sge:
      v = !slt;
      break;
    }
    if (ok)
      r = {v & mask, 1, 0};
    out.push_back(r);
  }
  return out;
}
std::vector<uint32_t> reachable(const Graph &g,
                                const std::vector<uint32_t> &seeds) {
  validate_graph(g);
  if (seeds.size() != g.nodes())
    throw std::invalid_argument("seed flags must match graph nodes");
  std::vector<uint32_t> result(g.nodes(), 0), work;
  for (uint32_t i = 0; i < seeds.size(); ++i)
    if (seeds[i]) {
      result[i] = 1;
      work.push_back(i);
    }
  while (!work.empty()) {
    uint32_t v = work.back();
    work.pop_back();
    for (uint32_t e = g.offsets[v]; e < g.offsets[v + 1]; ++e) {
      auto u = g.edges[e];
      if (!result[u]) {
        result[u] = 1;
        work.push_back(u);
      }
    }
  }
  return result;
}
std::vector<uint32_t> dominators(const Graph &g, uint32_t entry) {
  validate_graph(g);
  const size_t n = g.nodes();
  if (!n) {
    if (entry != 0)
      throw std::invalid_argument("entry outside empty graph");
    return {};
  }
  if (entry >= n)
    throw std::invalid_argument("dominator entry out of range");
  size_t words = (n + 31) / 32;
  if (n > std::numeric_limits<size_t>::max() / words)
    throw std::length_error("dominator matrix overflow");
  std::vector<uint32_t> seeds(n, 0);
  seeds[entry] = 1;
  auto reach = reachable(g, seeds);
  std::vector<std::vector<uint32_t>> pred(n);
  for (uint32_t v = 0; v < n; ++v)
    for (uint32_t e = g.offsets[v]; e < g.offsets[v + 1]; ++e)
      pred[g.edges[e]].push_back(v);
  std::vector<uint32_t> mask(words, 0), cur(n * words, 0), next(cur.size());
  for (size_t v = 0; v < n; ++v)
    if (reach[v])
      mask[v / 32] |= uint32_t(1) << (v % 32);
  for (size_t v = 0; v < n; ++v)
    if (reach[v])
      for (size_t w = 0; w < words; ++w)
        cur[v * words + w] =
            v == entry ? (w == v / 32 ? uint32_t(1) << (v % 32) : 0) : mask[w];
  bool changed;
  do {
    changed = false;
    next = cur;
    for (size_t v = 0; v < n; ++v)
      if (v != entry && reach[v])
        for (size_t w = 0; w < words; ++w) {
          uint32_t bits = UINT32_MAX;
          for (auto p : pred[v])
            if (reach[p])
              bits &= cur[p * words + w];
          if (w == v / 32)
            bits |= uint32_t(1) << (v % 32);
          next[v * words + w] = bits;
          changed |= bits != cur[v * words + w];
        }
    cur.swap(next);
  } while (changed);
  return cur;
}
std::vector<uint32_t> liveness(const Graph &g, uint32_t words,
                               const std::vector<uint32_t> &gen,
                               const std::vector<uint32_t> &kill) {
  validate_graph(g);
  if (words && g.nodes() > std::numeric_limits<size_t>::max() / words)
    throw std::length_error("liveness matrix overflow");
  size_t cells = g.nodes() * words;
  if (gen.size() != cells || kill.size() != cells)
    throw std::invalid_argument("liveness bitsets have wrong dimensions");
  std::vector<uint32_t> cur(cells, 0), next(cells, 0);
  bool changed;
  do {
    changed = false;
    for (uint32_t v = 0; v < g.nodes(); ++v)
      for (uint32_t w = 0; w < words; ++w) {
        uint32_t bits = 0;
        for (uint32_t e = g.offsets[v]; e < g.offsets[v + 1]; ++e)
          bits |= cur[size_t(g.edges[e]) * words + w];
        size_t i = size_t(v) * words + w;
        bits = gen[i] | (bits & ~kill[i]);
        next[i] = bits;
        changed |= bits != cur[i];
      }
    cur.swap(next);
  } while (changed);
  return cur;
}
std::vector<uint32_t> exclusive_scan(const std::vector<uint32_t> &in) {
  std::vector<uint32_t> out(in.size());
  uint32_t total = 0;
  for (size_t i = 0; i < in.size(); ++i) {
    out[i] = total;
    total += in[i];
  }
  return out;
}
std::vector<uint32_t> sort_keys(const std::vector<uint32_t> &in) {
  auto out = in;
  std::sort(out.begin(), out.end());
  return out;
}
std::vector<uint32_t> hash_records(const std::vector<uint32_t> &words,
                                   const std::vector<uint32_t> &offsets) {
  if (offsets.empty()) {
    if (words.empty())
      return {};
    throw std::invalid_argument("hash offsets are empty");
  }
  if (words.size() > UINT32_MAX || offsets.size() - 1 > UINT32_MAX)
    throw std::length_error("hash input exceeds 32-bit indices");
  if (offsets.front() != 0 || offsets.back() != words.size())
    throw std::invalid_argument("invalid hash offsets");
  for (size_t i = 1; i < offsets.size(); ++i)
    if (offsets[i] < offsets[i - 1])
      throw std::invalid_argument("nonmonotonic hash offsets");
  std::vector<uint32_t> hashes;
  hashes.reserve(offsets.size() - 1);
  for (size_t i = 0; i + 1 < offsets.size(); ++i) {
    if (offsets[i + 1] < offsets[i])
      throw std::invalid_argument("nonmonotonic hash offsets");
    uint32_t h = 2166136261u;
    for (uint32_t j = offsets[i]; j < offsets[i + 1]; ++j)
      for (uint32_t b = 0; b < 4; ++b)
        h = (h ^ ((words[j] >> (b * 8)) & 255u)) * 16777619u;
    hashes.push_back(h);
  }
  return hashes;
}
} // namespace lv::cpu
