// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/fabric_reconciliation/explain.hpp"

namespace summon {
namespace fabric_reconciliation {

void Explanation::Add(ReasonCode code, std::string detail) {
  if (steps_.size() >= max_steps_) {
    truncated_ = true;
    return;
  }
  ExplanationStep step;
  step.code = code;
  step.detail = std::move(detail);
  steps_.push_back(std::move(step));
}

void Explanation::Add(ReasonCode code) { Add(code, std::string()); }

bool Explanation::Has(ReasonCode code) const noexcept { return Find(code) != nullptr; }

const std::string* Explanation::Find(ReasonCode code) const noexcept {
  for (const ExplanationStep& step : steps_) {
    if (step.code == code) {
      return &step.detail;
    }
  }
  return nullptr;
}

std::string Explanation::Render() const {
  std::string result;
  for (const ExplanationStep& step : steps_) {
    result += ToText(step.code);
    if (!step.detail.empty()) {
      result += ": ";
      result += step.detail;
    }
    result += "\n";
  }
  if (truncated_) {
    result += "EXPLANATION_TRUNCATED\n";
  }
  return result;
}

}  // namespace fabric_reconciliation
}  // namespace summon
