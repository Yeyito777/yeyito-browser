// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/yeyito_hints/labels.h"

#include <string>

namespace blink::hint_labels {

wtf_size_t LabelLengthForCandidateCount(wtf_size_t count) {
  constexpr wtf_size_t kAlphabetSize = sizeof(kAlphabet) - 1;
  wtf_size_t length = 1;
  wtf_size_t capacity = kAlphabetSize;
  while (count > capacity) {
    ++length;
    capacity *= kAlphabetSize;
  }
  return length;
}

String LabelForIndex(wtf_size_t index, wtf_size_t length) {
  constexpr wtf_size_t kAlphabetSize = sizeof(kAlphabet) - 1;
  std::string label(length, kAlphabet[0]);
  for (wtf_size_t i = 0; i < length; ++i) {
    label[length - i - 1] = kAlphabet[index % kAlphabetSize];
    index = index / kAlphabetSize;
  }
  return String::FromUTF8(label);
}

}  // namespace blink::hint_labels
