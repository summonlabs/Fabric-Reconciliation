// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Explain surfaces.
//
// Every decision carries an ordered, bounded list of reason steps. The
// dry-run surface is the same decision data with mutation suppressed, so an
// explanation can never describe a plan that differs from the one that would
// actually be issued.

#ifndef SUMMON_FABRIC_RECONCILIATION_EXPLAIN_HPP
#define SUMMON_FABRIC_RECONCILIATION_EXPLAIN_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "summon/fabric_reconciliation/canonical.hpp"
#include "summon/fabric_reconciliation/identity.hpp"
#include "summon/fabric_reconciliation/limits.hpp"
#include "summon/fabric_reconciliation/platform.hpp"
#include "summon/fabric_reconciliation/status.hpp"

namespace summon {
namespace fabric_reconciliation {

/// One ordered explanation step. Detail text is bounded by the producer.
struct FR_API ExplanationStep {
  ReasonCode code{ReasonCode::None};
  std::string detail;

  friend bool operator==(const ExplanationStep&, const ExplanationStep&) = default;
};

/// Ordered, bounded explanation. Appending beyond the bound sets a truncated
/// flag instead of growing without limit.
class FR_API Explanation {
 public:
  Explanation() = default;
  explicit Explanation(std::size_t max_steps) : max_steps_(max_steps == 0 ? 1 : max_steps) {}

  void Add(ReasonCode code, std::string detail);
  void Add(ReasonCode code);

  [[nodiscard]] bool truncated() const noexcept { return truncated_; }
  [[nodiscard]] std::size_t max_steps() const noexcept { return max_steps_; }
  [[nodiscard]] const std::vector<ExplanationStep>& steps() const noexcept { return steps_; }
  [[nodiscard]] bool Has(ReasonCode code) const noexcept;
  [[nodiscard]] const std::string* Find(ReasonCode code) const noexcept;

  /// Deterministic multi-line rendering: one step per line, "CODE: detail".
  [[nodiscard]] std::string Render() const;

  friend bool operator==(const Explanation&, const Explanation&) = default;

 private:
  std::vector<ExplanationStep> steps_;
  std::size_t max_steps_{16};
  bool truncated_{false};
};

}  // namespace fabric_reconciliation
}  // namespace summon

#endif  // SUMMON_FABRIC_RECONCILIATION_EXPLAIN_HPP
