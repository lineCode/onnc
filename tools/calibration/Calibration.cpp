//===- Calibration.cpp ----------------------------------------------------===//
//
//                             The ONNC Project
//
// See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include <caffe/proto/caffe.pb.h>
#include <onnc/ADT/Color.h>
#include <onnc/Core/ModulePass.h>
#include <onnc/IR/ONNXUtils.h>
#include <onnc/Support/IOStream.h>

#include "Calibration.h"
#include "Kld.h"

#include <cmath>
#include <fstream>
#include <streambuf>
#include <string>
#include <type_traits>

#include <caffe2/core/db.h>
#include <caffe2/onnx/backend.h>

using namespace onnc;
using namespace caffe2;

namespace {

void copyTensor(::onnx::Tensor &pDstTensor, const ::onnx::Tensor &pSrcTensor,
                const std::string &pValueName,
                const ::onnx::TensorProto_DataType pDataType)
{
  // copy sizes
  auto &newTensorSize = pDstTensor.sizes();
  newTensorSize = pSrcTensor.sizes();
  // copy dataType
  auto &newTensorType = pDstTensor.elem_type();
  newTensorType = pDataType;
  // copy segment info
  if (pSrcTensor.is_segment()) {
    pDstTensor.set_segment_begin_and_end(pSrcTensor.segment_begin(),
                                         pSrcTensor.segment_end());
  }
  // copy name
  if (pSrcTensor.hasName()) {
    pDstTensor.setName(pSrcTensor.name());
  }
}

template <class T>
void copyData2Tensor(::onnx::Tensor &pTensor, const std::vector<T> &pDataVector)
{
  size_t size = pDataVector.size() * sizeof(T);
  std::string rawData(reinterpret_cast<const char *>(pDataVector.data()), size);
  pTensor.set_raw_data(rawData);
}

const std::vector<int64_t>
getDimension(const std::vector< ::onnx::Dimension> pDims)
{
  std::vector<int64_t> int64Dim;
  for (auto &dim : pDims) {
    int64Dim.push_back(dim.dim);
  }
  return int64Dim;
}

const std::vector<int64_t> getInputDataDim(const ::onnx::Graph &pConstGraph)
{
  // FIXME
  ::onnx::Graph &graph = const_cast< ::onnx::Graph &>(pConstGraph);
  std::unordered_set<std::string> initializerNames(
      graph.initializer_names().begin(), graph.initializer_names().end());

  std::vector<const ::onnx::Value *> inputDatas;
  for (const ::onnx::Value *v : graph.inputs()) {
    if (0 == initializerNames.count(v->uniqueName())) {
      inputDatas.push_back(v);
    }
  }
  assert(1 == inputDatas.size());
  return getDimension(inputDatas[0]->sizes());
}

} // anonymous namespace

namespace onnc {
static int getRightShift(Blob *pBlob, float pScale)
{
  int m = 0;
  auto tensor = pBlob->Get<TensorCPU>();
  if (tensor.IsType<float>()) {
    const auto &probs = tensor.data<float>();
    auto size = tensor.size();
    // Find max abs in the tensor.
    float max = *std::max_element(probs, probs + size);
    float min = *std::max_element(probs, probs + size);
    float abs_max = (std::abs(min) > max) ? std::abs(min) : max;
    float data_max = abs_max * pScale;
    if (data_max <= 0) {
      std::cout << "data_max = " << data_max << std::endl;
      throw std::runtime_error("data_max <= 0");
    }
    while (data_max < 64) {
      m += 1;
      data_max *= 2;
    }
    while (data_max >= 128) {
      std::cout << "data_max = " << data_max << std::endl;
      std::cout << "Error in quantize_right_shift: rshift will be negative..."
                << data_max << std::endl;
      exit(-1);
    }
    return m;
  } else {
    // Assert.
    throw std::runtime_error("Blob format is not float!");
  }
  return m;
}

template <std::size_t SIZE>
static int calRightShift(const std::array<float, SIZE> &pData, float pScale)
{
  float max{ 0.0 };
  for (auto &d : pData) {
    float v = fabs(d);
    if (v > max)
      max = v;
  }
  max *= pScale;

  if (max <= 0)
    throw std::runtime_error("Error: calRightShift: max <= 0");
  if (max >= 128)
    throw std::runtime_error("Error: calRightShift: max > 128");

  int s = 0;
  while (max < 64) {
    s += 1;
    max *= 2;
  }
  return s;
}

template <class T> static inline T saturate(int pValue)
{
  const int32_t max = std::numeric_limits<T>::max();
  const int32_t min = std::numeric_limits<T>::min();

  T sValue = std::min(max, pValue);
  sValue = std::max(min, pValue);

  return sValue;
}

static bool isBlobUsed(const string &pBlobName, caffe2::NetDef &pDef,
                       const int pOpIdx)
{
  bool start = false;
  int opIndex = 0;
  for (const OperatorDef &op : pDef.op()) {
    for (const string &in : op.input()) {
      if (start && (in == pBlobName)) {
        return true;
      }
      if ((in == pBlobName) && (opIndex == pOpIdx)) {
        start = true;
      }
    }
    opIndex++;
  }
  return false;
}

static bool isBlobSingleUser(const string &pBlobName, caffe2::NetDef &pDef)
{
  int useCount = 0;
  for (const OperatorDef &op : pDef.op()) {
    for (const string &in : op.input()) {
      if (in == pBlobName) {
        useCount++;
        if (useCount > 1) {
          return false;
        }
      }
    }
  }
  return true;
}

static int getMultiplier(float pX, float pY, int *pRightShift)
{
  float denominatorCeiling = 256.0 / pX * pY;
  int rightShift = 0;
  float denominatorQuantized = 1.0;

  while (denominatorQuantized * 2 < denominatorCeiling) {
    rightShift += 1;
    denominatorQuantized = float(1 << rightShift);
  }

  *pRightShift = rightShift;
  int multiplier = (int)floor(((pX / pY) * denominatorQuantized) + 0.5);

  return multiplier;
}

template <class T>
void Calibration::quantizeWeight(Blob *pBlob, float pThresX, float pThresY,
                                 int pRightShift, string pName)
{
  auto tensor = pBlob->Get<TensorCPU>();
  int shiftScale = 1 << pRightShift;
  if (tensor.IsType<float>()) {
    const auto &probs = tensor.data<float>();
    auto size = tensor.size();
    if (std::is_same<T, int8_t>::value) {
      m_QWeights[pName].reserve(size);
    } else if (std::is_same<T, int16_t>::value) {
      m_QBias[pName].reserve(size);
    } else {
      throw std::runtime_error("Quantized type not support!");
    }

    for (int i = 0; i < tensor.size(); i++) {
      float fWeight =
          floor(probs[i] * ((pThresX / pThresY) * shiftScale) + 0.5);
      T qWeight = saturate<T>((int)fWeight);

      if (std::is_same<T, int8_t>::value) {
        m_QWeights[pName].emplace_back(qWeight);
      } else if (std::is_same<T, int16_t>::value) {
        m_QBias[pName].emplace_back(qWeight);
      }
    }
  } else {
    throw std::runtime_error("Blob format is not float!");
  }
}

bool Calibration::readDataset(const std::vector<int64_t> &pInputDims,
                              const string &pDataLayer, int pIteration)
{
  auto nums = ::onnc::getTotalCount(pInputDims);
  auto reader = std::make_unique<caffe2::db::DBReader>("lmdb", m_DBName);
  auto *curCursor = reader->cursor();
  curCursor->SeekToFirst();
  for (int run = 0; run < pIteration; run++) {
    std::cout << "pInputDims NCHW: " << pInputDims << std::endl;
    std::vector<float> data;
    for (int b = 0; b < pInputDims.at(0); b++) {
      std::string raw(curCursor->value());
      curCursor->Next();

      caffe::Datum datum;
      datum.ParseFromString(raw);
      std::cout << "datum CHW: " << datum.channels() << " " << datum.height()
                << " " << datum.width() << std::endl;
      int h = datum.height();
      int w = datum.width();
      int h_crop = pInputDims.at(2);
      int w_crop = pInputDims.at(3);
      assert(h == w);
      assert(h_crop == w_crop);
      assert(datum.channels() == pInputDims.at(1));
      int h_offset = (h - h_crop) / 2;
      int w_offset = (w - w_crop) / 2;
      bool is_vgg16 = false;
      std::vector<float> mean = { 103.939, 116.779, 123.68 };
      for (int c = 0; c < datum.channels(); ++c) {
        for (int ih = h_offset; ih < h_offset + h_crop; ++ih) {
          for (int iw = w_offset; iw < w_offset + w_crop; ++iw) {
            int i = c * h * w + ih * w + iw;
            float pixel = (uint8_t)datum.data()[i];
            if (is_vgg16)
              pixel -= mean[c];
            else
              pixel /= 256; // FIXME: The scale "1/256" is only for mnist.
            data.push_back(pixel);
          }
        }
      }
    }
    if (nums != data.size()) {
      std::cout << "error: dimension mismatch, " << nums << " vs "
                << data.size() << std::endl;
      exit(1);
    }
    TensorCPU tensor(pInputDims, data, nullptr);
    m_BlobData[pDataLayer].emplace_back(tensor);
  }

  return true;
}

void Calibration::updateQuantizeWeight(::onnx::Graph *pGraph)
{
  // update elemType
  for (auto input : pGraph->inputs()) {
    auto elemType = input->elemType();
    if (elemType == ::onnx::TensorProto_DataType_FLOAT) {
      auto name = input->uniqueName();
      if (m_QWeights.count(name)) {
        input->setElemType(::onnx::TensorProto_DataType_INT8);
      } else if (m_QBias.count(name)) {
        input->setElemType(::onnx::TensorProto_DataType_INT16);
      } else {
        // input data default is INT8
        input->setElemType(::onnx::TensorProto_DataType_INT8);
      }
    } else {
      // FIXME
      std::cout << "FIXME: unsupported quantize type:"
                << TensorProto_DataType_Name(elemType) << std::endl;
    }
  }

  // update node's output elementType
  for (::onnx::Node *node : pGraph->nodes()) {
    for (::onnx::Value *out : node->outputs()) {
      out->setElemType(::onnx::TensorProto_DataType_INT8);
    }
  }

  // update Tensor
  std::unordered_map<std::string, ::onnx::Tensor> valueTensorMap;
  const std::vector< ::onnx::Tensor> initTensors = pGraph->initializers();
  const std::vector<std::string> tensorNames = pGraph->initializer_names();
  for (size_t i = 0; i < initTensors.size(); ++i) {
    auto valueName = tensorNames[i];
    auto oldTensor = initTensors[i];
    if (1 == m_QWeights.count(valueName)) {
      ::onnx::Tensor newTensor;
      copyTensor(newTensor, oldTensor, valueName,
                 ::onnx::TensorProto_DataType_INT8);
      assert(m_QWeights[valueName].size() ==
             ::onnc::getTotalCount(oldTensor.sizes()));
      copyData2Tensor(newTensor, m_QWeights[valueName]);
      valueTensorMap.emplace(valueName, newTensor);
      continue;
    } else if (1 == m_QBias.count(valueName)) {
      ::onnx::Tensor newTensor;
      copyTensor(newTensor, oldTensor, valueName,
                 ::onnx::TensorProto_DataType_INT16);
      assert(m_QBias[valueName].size() ==
             ::onnc::getTotalCount(oldTensor.sizes()));
      copyData2Tensor(newTensor, m_QBias[valueName]);
      valueTensorMap.emplace(valueName, newTensor);
      continue;
    }
    // FIXME
    std::cout << "FIXME: unsupported tensor:" << oldTensor.name() << std::endl;
    valueTensorMap.insert({ valueName, oldTensor });
  }

  pGraph->clearInitializers();
  for (auto &kv : valueTensorMap) {
    pGraph->addInitializer(kv.second, kv.first);
  }
}

float Calibration::calculateKLD(const string &pBlobName)
{
  int iteration = m_BlobData[pBlobName].size();
  int dataSize = 0;
  vector<float> dataCollect;
  // Count memory size to reserve.
  for (int i = 0; i < iteration; i++) {
    dataSize += m_BlobData[pBlobName][i].size();
  }
  dataCollect.reserve(dataSize);
  // Concat all batches' tensor.
  for (int i = 0; i < iteration; i++) {
    auto probs = m_BlobData[pBlobName][i].data<float>();
    dataCollect.insert(dataCollect.end(), probs,
                       probs + m_BlobData[pBlobName][i].size());
  }
  return KLDiversity(dataCollect.data(), dataCollect.size());
}

void Calibration::profileModel(int pIteration, caffe2::NetDef &pDef,
                               const string &pDataLayer)
{
  // Calculate "data layer" KLD.
  m_ThresholdY[pDataLayer] = calculateKLD(pDataLayer);

  // Add "data layer" threshold into Ctable.
  tg::bm1880::LayerCalibrationParameter *layerCalibrationParam =
      m_NetCtableParam.add_layer();
  layerCalibrationParam->set_name(pDataLayer);
  tg::bm1880::BlobParameter *outBlobParam =
      layerCalibrationParam->add_blob_param();
  outBlobParam->set_name(pDataLayer);
  outBlobParam->set_threshold_y(m_ThresholdY[pDataLayer]);

  int opIdx = 0;
  for (const OperatorDef &op : pDef.op()) {
    for (int run = 0; run < pIteration; run++) {
      // Prepare input blobs.
      for (const string &in : op.input()) {
        // Only feed data to input. (no needs to feed weights)
        if (m_BlobData.find(in) != m_BlobData.end()) {
          // Feed data from previous layer.
          auto input = m_Workspace->GetBlob(in)->GetMutable<TensorCPU>();
          input->ResizeLike(m_BlobData[in][run]);
          input->ShareData(m_BlobData[in][run]);
        }
      }
      CAFFE_ENFORCE(m_Workspace->RunOperatorOnce(op));
      // Save output blobs.
      for (const string &in : op.output()) {
        auto blob = m_Workspace->GetBlob(in);
        // Special case: Skip Dropout layer.
        // FIXME: Remove. Dropout layer should be deleted in layer-opt. phase.
        if (string(blob->TypeName()) != "nullptr (uninitialized)") {
          CPUContext cpuContext;
          TensorCPU output(m_Workspace->GetBlob(in)->Get<TensorCPU>(),
                           &cpuContext);
          m_BlobData[in].emplace_back(output);
        }
      }
    }
    // Calculate KLD. Save results in threshold_y.
    for (const string &in : op.output()) {
      auto blob = m_Workspace->GetBlob(in);
      // FIXME: Remove. Dropout layer should be deleted in layer-opt. phase.
      if (string(blob->TypeName()) != "nullptr (uninitialized)" &&
          blob->Get<TensorCPU>().IsType<float>()) {
        m_ThresholdY[in] = calculateKLD(in);
      }
    }

    // Free unused input blobs for saving memory.
    for (const string &in : op.input()) {
      if (m_BlobData.find(in) != m_BlobData.end()) {
        if (!isBlobUsed(in, pDef, opIdx)) {
          m_BlobData.erase(in);
        }
      }
    }
    opIdx++;
  }
}

#include "LayerImpl.h"
void Calibration::getRightShiftQuantize(caffe2::NetDef &pDef)
{
  for (const OperatorDef &op : pDef.op()) {
    tg::bm1880::LayerCalibrationParameter *layerCalibrationParam =
        m_NetCtableParam.add_layer();
    layerCalibrationParam->set_name(op.output(0));
    for (const string &out : op.output()) {
      tg::bm1880::BlobParameter *outBlobParam =
          layerCalibrationParam->add_blob_param();
      outBlobParam->set_name(out);
      outBlobParam->set_threshold_y(m_ThresholdY[out]);
    }

    if (op.type() == "Conv" || op.type() == "FC" || op.type() == "Scale") {
      Conv(op, pDef, layerCalibrationParam);
    } else if (op.type() == "MaxPool" || op.type() == "AveragePool") {
      Pool(op, pDef, layerCalibrationParam);
    } else if (op.type() == "Sum" || op.type() == "Max" || op.type() == "Mul") {
      Eltwise(op, pDef, layerCalibrationParam);
    } else if (op.type() == "Relu" || op.type() == "Flatten" ||
               op.type() == "Concat" || op.type() == "Reshape") {
      // Do nothing.
    } else {
      // FIXME: Add assert in the future.
      errs() << Color::RED << "Error" << Color::RESET << ": Unsupport op type "
             << op.type() << std::endl;
    }
    // TODO: Add other layers.
  }
}

// Optimizations for Quantization.
void Calibration::thresholdFold(caffe2::NetDef &pDef)
{
  // Forward folding.
  for (const OperatorDef &op : pDef.op()) {
    if (op.type() == "Relu" || op.type() == "Flatten") {
      const string &inputName = op.input(0);
      const string &outputName = op.output(0);
      m_ThresholdY[outputName] = m_ThresholdY[inputName];
    }
  }

  // Backward folding.
  int opSize = pDef.op_size();
  for (int i = (opSize - 1); i >= 0; i--) {
    const OperatorDef &op = pDef.op(i);

    if (op.type() == "Relu" || op.type() == "Flatten") {
      const string &inputName = op.input(0);
      const string &outputName = op.output(0);
      m_ThresholdY[inputName] = m_ThresholdY[outputName];
    }

    if (op.type() == "Concat") {
      const string &outputName = op.output(0);
      // Check the bottom layer's output is not only for concat.
      for (const string &in : op.input()) {
        if (isBlobSingleUser(in, pDef)) {
          m_ThresholdY[in] = m_ThresholdY[outputName];
        } else {
          std::cout << "Blob " << in << " of Concat input has multiple user!"
                    << std::endl;
          assert(0);
        }
      }
    }
  }
}

Pass::ReturnType Calibration::runOnModule(::onnc::Module &pModule)
{
  // TODO: Check If Ctable exist, then skip this pass.

  std::string onnxStr;
  ::onnc::SerializeToString(onnxStr, pModule);

  // Create caffe2 backend.
  auto *backend = new caffe2::onnx::Caffe2Backend(nullptr);
  std::vector<caffe2::onnx::Caffe2Ops> extras;
  auto rep = backend->Prepare(onnxStr, "CPU", extras);
  auto def = rep->pred_net();

  // Init net weights.
  m_Workspace = new Workspace("layer_calibration");
  CAFFE_ENFORCE(m_Workspace->RunNetOnce(rep->init_net()));

  // Find data layer's name.
  const OperatorDef &op = def.op(0);
  const string &dataLayer = op.input(0);
  m_Workspace->CreateBlob(dataLayer);
  auto graph = pModule.getGraphIR();
  if (!readDataset(getInputDataDim(*graph.get()), dataLayer, m_Iteration)) {
    errs() << Color::RED << "Error" << Color::RESET << ": Read data set fail..."
           << std::endl;
    return Pass::kModuleNoChanged;
  }

  // Run inference and calculate KLD.
  profileModel(m_Iteration, def, dataLayer);

  thresholdFold(def);

  // Caliculate right-shift each layer and Quantize weights.
  getRightShiftQuantize(def);

  std::cout << m_NetCtableParam.DebugString() << std::endl;
  // write ctable
  pModule.getMetaData().insert(
      { "bm1880_ctable", m_NetCtableParam.DebugString() });
  // write qWeights
  updateQuantizeWeight(pModule.getGraphIR().get());

  delete backend;

  return Pass::kModuleChanged;
}
} // namespace onnc

char Calibration::ID = 0;
ModulePass *onnc::createCalibrationPass(const std::string pDBName, int pI)
{
  return new Calibration(pDBName, pI);
}