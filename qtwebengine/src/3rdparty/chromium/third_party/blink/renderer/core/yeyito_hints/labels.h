// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_YEYITO_HINTS_LABELS_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_YEYITO_HINTS_LABELS_H_

#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink::hint_labels {

inline constexpr char kAlphabet[] = "asdfghjklqwertyuiopzxcvbnm";

wtf_size_t LabelLengthForCandidateCount(wtf_size_t count);
String LabelForIndex(wtf_size_t index, wtf_size_t length);

}  // namespace blink::hint_labels

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_YEYITO_HINTS_LABELS_H_
