#pragma once

#include "BM168xBackend.h"
#include "TargetLowering.h"
#include <memory>
#include <onnx/common/ir.h>

namespace onnc {

class BM168xBackend;

class BM168xTargetLowering : public TargetLowering
{
public:
  BM168xTargetLowering(TGBackend *pBackend) : TargetLowering(pBackend) {}

  void
  LowerOperation(const ::onnx::Node &node,
                 std::vector<std::unique_ptr<Operator> > &instList) override;

private:
  Operator *LowerHelper(const ::onnx::Node &node);
};

} // namespace onnc
