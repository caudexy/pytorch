#include "lazy_tensor_core/csrc/aten_ltc_bridge.h"

#include <map>
#include <string>
#include <vector>

#include "lazy_tensor_core/csrc/device.h"
#include "lazy_tensor_core/csrc/lazy_graph_executor.h"
#include "lazy_tensor_core/csrc/tensor_impl.h"
#include "lazy_tensor_core/csrc/torch_util.h"

namespace torch_lazy_tensors {
namespace bridge {
namespace {

LTCTensorImpl* GetLtcTensorImpl(const at::Tensor& tensor) {
  return dynamic_cast<LTCTensorImpl*>(tensor.unsafeGetTensorImpl());
}

}  // namespace

c10::optional<LazyTensor> TryGetLtcTensor(const at::Tensor& tensor) {
  LTCTensorImpl* impl = GetLtcTensorImpl(tensor);
  if (impl == nullptr) {
    return c10::nullopt;
  }
  return impl->tensor();
}

bool IsLtcTensor(const at::Tensor& tensor) {
  return GetLtcTensorImpl(tensor) != nullptr;
}

LazyTensor GetLtcTensor(const at::Tensor& tensor) {
  auto xtensor = TryGetLtcTensor(tensor);
  CHECK(xtensor) << "Input tensor is not a lazy tensor: " << tensor.toString();
  return *xtensor;
}

void ReplaceLtcTensor(const at::Tensor& tensor, LazyTensor new_ltc_tensor) {
  LTCTensorImpl* impl =
      dynamic_cast<LTCTensorImpl*>(tensor.unsafeGetTensorImpl());
  CHECK(impl != nullptr) << "Input tensor is not a lazy tensor: "
                         << tensor.toString();
  impl->set_tensor(std::move(new_ltc_tensor));
}

std::vector<LazyTensor> GetLtcTensors(c10::ArrayRef<at::Tensor> tensors) {
  std::vector<LazyTensor> ltc_tensors;
  ltc_tensors.reserve(tensors.size());
  for (const auto& tensor : tensors) {
    ltc_tensors.push_back(bridge::GetLtcTensor(tensor));
  }
  return ltc_tensors;
}

LazyTensor GetOrCreateLtcTensor(const at::Tensor& tensor,
                                const Device& device) {
  if (!tensor.defined()) {
    return LazyTensor();
  }
  auto xtensor = TryGetLtcTensor(tensor);
  return xtensor ? *xtensor : LazyTensor::Create(tensor, device);
}

LazyTensor GetOrCreateLtcTensor(const c10::optional<at::Tensor>& tensor,
                                const Device& device) {
  if (!IsDefined(tensor)) {
    return LazyTensor();
  }
  auto xtensor = TryGetLtcTensor(*tensor);
  return xtensor ? *xtensor : LazyTensor::Create(*tensor, device);
}

LazyTensor GetLtcTensorOrCreateForWrappedNumber(const at::Tensor& tensor, const Device& device) {
  return tensor.unsafeGetTensorImpl()->is_wrapped_number() ?
      GetOrCreateLtcTensor(tensor, device) : GetLtcTensor(tensor);
}

std::vector<at::Tensor> LtcCreateTensorList(const at::TensorList& tensors) {
  std::vector<at::Tensor> aten_ltc_tensors(tensors.size());
  std::vector<LazyTensor> ltc_tensors;
  // We need to separate out the defined tensors first, GetLtcTensor() doesn't
  // work with undefined tensors.
  std::vector<bool> to_translate(tensors.size());
  for (size_t i = 0; i < tensors.size(); ++i) {
    const at::Tensor& tensor = tensors[i];
    if (tensor.defined()) {
      auto xtensor = TryGetLtcTensor(tensor);
      if (xtensor) {
        to_translate[i] = true;
        ltc_tensors.push_back(*xtensor);
      } else {
        aten_ltc_tensors[i] = tensor;
      }
    }
  }
  auto defined_aten_ltc_tensors =
      LazyGraphExecutor::Get()->GetTensors(&ltc_tensors);
  // Insert undefined tensors into the result, back into the original undefined
  // positions.
  for (size_t i = 0, defined_pos = 0; i < tensors.size(); ++i) {
    if (to_translate[i]) {
      aten_ltc_tensors[i] = std::move(defined_aten_ltc_tensors[defined_pos++]);
    }
  }
  return aten_ltc_tensors;
}

std::vector<c10::optional<at::Tensor>> LtcCreateOptTensorList(
    const std::vector<c10::optional<at::Tensor>>& tensors) {
  std::vector<c10::optional<at::Tensor>> opt_aten_ltc_tensors(tensors.size());
  std::vector<at::Tensor> materialized_tensors;
  std::vector<bool> to_translate(tensors.size());
  for (size_t i = 0; i < tensors.size(); ++i) {
    auto tensor = tensors[i];
    if (tensor.has_value()) {
      to_translate[i] = true;
      materialized_tensors.push_back(*tensor);
    }
  }
  auto aten_materialzied_tensors = LtcCreateTensorList(materialized_tensors);
  for (size_t i = 0, defined_pos = 0; i < tensors.size(); ++i) {
    if (to_translate[i]) {
      opt_aten_ltc_tensors[i] =
          std::move(aten_materialzied_tensors[defined_pos++]);
    }
  }
  return opt_aten_ltc_tensors;
}

void LtcUpdateTensors(c10::ArrayRef<at::Tensor> dest_ltc_tensors,
                      c10::ArrayRef<at::Tensor> source_cpu_tensors,
                      c10::ArrayRef<size_t> indices) {
  for (auto index : indices) {
    at::Tensor dest = dest_ltc_tensors.at(index);
    at::Tensor source = source_cpu_tensors.at(index);
    LTCTensorImpl* dest_impl = GetLtcTensorImpl(dest);
    if (dest_impl != nullptr) {
      auto ltc_source = TryGetLtcTensor(source);
      if (!ltc_source) {
        dest_impl->tensor().UpdateFromTensorOut(source);
      } else {
        dest_impl->tensor().UpdateFromTensorOut(*ltc_source);
      }
      dest_impl->force_refresh_sizes();
    } else {
      dest.resize_as_(source).copy_(source);
    }
  }
}

void LtcUpdateTensorsMeta(c10::ArrayRef<at::Tensor> dest_ltc_tensors,
                          c10::ArrayRef<at::Tensor> source_cpu_tensors,
                          c10::ArrayRef<size_t> indices) {
  for (auto index : indices) {
    at::Tensor dest = dest_ltc_tensors.at(index);
    at::Tensor source = source_cpu_tensors.at(index);
    LTCTensorImpl* dest_impl = GetLtcTensorImpl(dest);
    CHECK(dest_impl);
    auto ltc_source = TryGetLtcTensor(source);
    CHECK(!ltc_source);
    LazyTensor& ltc_dest = dest_impl->tensor();
    ltc_dest.SetTensor(source);
  }
}

c10::optional<Device> GetLtcDevice(const at::Tensor& tensor) {
  auto xtensor = TryGetLtcTensor(tensor);
  if (!xtensor) {
    return c10::nullopt;
  }
  return xtensor->GetDevice();
}

c10::optional<Device> GetLtcDevice(const c10::optional<at::Tensor>& tensor) {
  if (!tensor.has_value()) {
    return c10::nullopt;
  }
  return GetLtcDevice(*tensor);
}

c10::optional<Device> GetLtcDevice(const at::TensorList& tensors) {
  for (const auto& tensor : tensors) {
    auto device = GetLtcDevice(tensor);
    if (device) {
      return device;
    }
  }
  return c10::nullopt;
}

c10::optional<Device> GetLtcDevice(const at::TensorOptions& tensor_options) {
  if (!tensor_options.has_device()) {
    return c10::nullopt;
  }
  return GetLtcDevice(tensor_options.device());
}

c10::optional<Device> GetLtcDevice(const c10::Device& device) {
  if (device.type() != at::kLazy) {
    return c10::nullopt;
  }
  return AtenDeviceToLtcDevice(device);
}

c10::optional<Device> GetLtcDevice(const c10::optional<c10::Device>& device) {
  if (!device) {
    return c10::nullopt;
  }
  return GetLtcDevice(*device);
}

Device AtenDeviceToLtcDevice(const c10::Device& device) {
  CHECK_EQ(device.type(), at::kLazy) << device;
  int ordinal = device.has_index() ? device.index() : -1;
  if (ordinal < 0) {
    c10::Device current_device = GetCurrentAtenDevice();
    if (current_device.has_index()) {
      ordinal = current_device.index();
    }
  }
  if (ordinal < 0) {
    return GetCurrentDevice();
  }
  return Device(compiler::getBackend()->GetDefaultDeviceType(), ordinal);
}

c10::Device LtcDeviceToAtenDevice(const Device& device) {
  return c10::Device(at::kLazy, device.ordinal());
}

std::string ToLtcString(const c10::Device& device) {
  return c10::str("lazy:", device.index());
}

c10::Device AtenDefaultDevice() {
  return LtcDeviceToAtenDevice(*GetDefaultDevice());
}

c10::Device SetCurrentDevice(const c10::Device& device) {
  Device prev_device =
      torch_lazy_tensors::SetCurrentDevice(AtenDeviceToLtcDevice(device));
  return LtcDeviceToAtenDevice(prev_device);
}

Device SetCurrentDevice(const Device& device) {
  return torch_lazy_tensors::SetCurrentDevice(device);
}

c10::Device GetCurrentAtenDevice() {
  return LtcDeviceToAtenDevice(torch_lazy_tensors::GetCurrentDevice());
}

at::Tensor LtcToAtenTensor(LazyTensor ltc_tensor,
                           const at::TensorOptions& tensor_options) {
  if (tensor_options.has_device()) {
    CHECK_NE(tensor_options.device().type(), at::kLazy);
  }
  at::Tensor tensor = ltc_tensor.ToTensor(/*detached=*/false);
  // We need to copy the tensor since it is cached within the LazyTensor, and
  // returning it directly might expose it to in place changes. Which there was
  // COW option :)
  return tensor.to(tensor_options, /*non_blocking=*/false, /*copy=*/true);
}

at::Tensor AtenFromLtcTensor(LazyTensor ltc_tensor) {
  return ltc_tensor.is_null() ? at::Tensor()
                              : at::Tensor(c10::make_intrusive<LTCTensorImpl>(
                                    std::move(ltc_tensor)));
}

std::vector<at::Tensor> AtenFromLtcTensors(
    c10::ArrayRef<LazyTensor> ltc_tensors) {
  std::vector<at::Tensor> tensors;
  tensors.reserve(ltc_tensors.size());
  for (auto& tensor : ltc_tensors) {
    tensors.emplace_back(AtenFromLtcTensor(tensor));
  }
  return tensors;
}

at::Tensor CreateLtcTensor(at::Tensor tensor,
                           const c10::optional<Device>& device) {
  if (tensor.defined() && device) {
    LazyTensor ltc_tensor = LazyTensor::Create(std::move(tensor), *device);
    tensor = AtenFromLtcTensor(ltc_tensor);
  }
  return tensor;
}

std::vector<at::Tensor> CreateLtcTensors(const std::vector<at::Tensor>& tensors,
                                         const c10::optional<Device>& device) {
  std::vector<at::Tensor> xtensors;
  for (auto& tensor : tensors) {
    xtensors.push_back(CreateLtcTensor(tensor, device));
  }
  return xtensors;
}

bool IsInteropView(const at::Tensor& t) {
  auto impl = dynamic_cast<const LTCTensorImpl*>(t.unsafeGetTensorImpl());
  return impl && impl->IsInteropView();
}

}  // namespace bridge
}  // namespace torch_lazy_tensors
