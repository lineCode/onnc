#include "BM188xBackend.h"
#include "BM188xISelLowering.h"
#include "TGConv.h"
#include "TGGemm.h"
#include "TGLRN.h"
#include "TGMaxPool.h"
#include "TGRelu.h"
#include "TGSoftmax.h"
#include "common_calibration.pb.h"

#define DEBUG_TYPE "bm1880_lowering"
#include <onnc/Support/Debug.h>

using namespace onnc;

ComputeOperand2 *BM188xISelLowering::LowerHelper(const ::onnx::Node &pNode)
{
  uint32_t symbol = pNode.kind();
  if (symbol == ::onnx::Symbol("Undefined"))
    return nullptr;

  std::string layerName =
      const_cast< ::onnx::Node &>(pNode).output()->uniqueName();
  const LayerCalibrationParameter &layerCtable =
      m_p1880backend->getCtableLayerParam(layerName);
  DEBUG(dbgs() << "layerName:" << layerName << "\n";);
  DEBUG(dbgs() << "LayerCalibrationParameter:" << layerCtable.DebugString(););
  if (symbol == ::onnx::Symbol("Conv"))
    return new BM188X::TGConv(pNode, layerCtable);
  else if (symbol == ::onnx::Symbol("Relu"))
    return new TGRelu(pNode);
  else if (symbol == ::onnx::Symbol("LRN"))
    return new TGLRN(pNode);
  else if (symbol == ::onnx::Symbol("MaxPool"))
    return new TGMaxPool(pNode);
  else if (symbol == ::onnx::Symbol("Gemm"))
    return new TGGemm(pNode);
  else if (symbol == ::onnx::Symbol("Softmax"))
    return new TGSoftmax(pNode);
  DEBUG(dbgs() << "unsupported node type: " << pNode.kind().toString()
               << std::endl;);
  return nullptr;
}

void BM188xISelLowering::LowerOperation(
    const ::onnx::Node &pNode,
    std::vector<std::unique_ptr<ComputeOperand2> > &pInstList)
{
  std::unique_ptr<ComputeOperand2> oper(LowerHelper(pNode));
  // FIXME ignore unsupported operation
  if (nullptr == oper)
    return;
  DEBUG(dbgs() << "lowering type: " << oper->getTypeName()
               << "\nlayer name:" << oper->getLayerName() << "\n";);
  pInstList.push_back(std::move(oper));
}

void BM188xISelLowering::PrepareISelLowering(Module &pModule)
{
  // prepare ctable
  auto &ctable = m_pBackend->getCtable(pModule);
  m_pBackend->setCtableProto(ctable);
}
