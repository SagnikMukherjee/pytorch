#pragma once

#include <c10/util/Metaprogramming.h>
#include <c10/util/TypeList.h>
#include <c10/core/TensorOptions.h>

namespace c10 {
namespace impl {

namespace detail {

// scatter_tensor_options takes a function pointer that potentially takes a TensorOptions argument.
// If it does, then it creates a new function pointer that takes scattered arguments, internally
// gathers those arguments, and then calls the underlying function pointer. If the underlying
// function pointer does not take a TensorOptions argument, it is passed through unmodified.
// Note that naming can be confusing here. scatter_tensor_options is a transformation on the function
// that scatters its parameters, but if seen in the call stack of a running program, it actually takes
// scattered arguments and gathers them into a TensorOptions object.
// GatheredParameterTypes: function parameters where TensorOptions is packed as a TensorOptions argument
// ScatteredParameterTypes: function parameters where TensorOptions doesn't exist but we have ScalarType, DeviceType and Layout parameters

template<class Type>
inline constexpr bool is_tensoroptions_arg() {
    return std::is_same<TensorOptions, std::decay_t<Type>>::value;
}
template<class Type>
using is_tensoroptions_arg_t = guts::bool_constant<is_tensoroptions_arg<Type>()>;

template<class FuncType>
inline constexpr bool has_tensoroptions_arg() {
    using parameter_types = typename guts::infer_function_traits_t<FuncType>::parameter_types;
    constexpr size_t num_tensoroptions_args = guts::typelist::count_if<is_tensoroptions_arg_t, parameter_types>::value;
    static_assert(num_tensoroptions_args <= 1, "Function has multiple TensorOptions parameters. We support at most one.");
    return num_tensoroptions_args > 0;
}

static_assert(has_tensoroptions_arg<int (int64_t, const TensorOptions&)>(), "");
static_assert(has_tensoroptions_arg<int (int64_t, TensorOptions)>(), "");
static_assert(!has_tensoroptions_arg<int (int64_t, std::string)>(), "");

template<class FuncType, FuncType* base_func_ptr, class ParametersBeforeTensorOptions, class ParametersAfterTensorOptions> struct scatter_tensor_options_;

template<class FuncType, FuncType* base_func_ptr, class Enable = void>
struct scatter_tensor_options final {};

template<class FuncType, FuncType* base_func_ptr>
struct scatter_tensor_options<FuncType, base_func_ptr, std::enable_if_t<!has_tensoroptions_arg<FuncType>()>> final {
    static constexpr auto* func_ptr() {
        // FuncType does not have TensorOptions arguments.
        // Don't wrap anything but just return the base pointer.
        // TODO Merge this case with the other one and use if_constexpr
        return base_func_ptr;
    }
};

template<class FuncType, FuncType* base_func_ptr>
struct scatter_tensor_options<FuncType, base_func_ptr, std::enable_if_t<has_tensoroptions_arg<FuncType>()>> final {
    static constexpr auto* func_ptr() {
        // FuncType has TensorOptions arguments.
        // Return a function pointer to a wrapper function that replaces those with expanded arguments.
        using gathered_parameter_types = typename guts::infer_function_traits_t<FuncType>::parameter_types;
        constexpr size_t tensoroptions_arg_index =
            guts::typelist::find_if<
                gathered_parameter_types,
                is_tensoroptions_arg_t
            >::value;

        using parameters_before_tensoroptions =
            guts::typelist::take_t<gathered_parameter_types, tensoroptions_arg_index>;
        using parameters_after_tensoroptions =
            guts::typelist::drop_t<gathered_parameter_types, tensoroptions_arg_index + 1>;

        constexpr auto result = &scatter_tensor_options_<FuncType, base_func_ptr, parameters_before_tensoroptions, parameters_after_tensoroptions>::wrapper;
        return result;
    }
};

template<class FuncType, FuncType* base_func_ptr, class... ParametersBeforeTensorOptions, class... ParametersAfterTensorOptions>
struct scatter_tensor_options_<FuncType, base_func_ptr, guts::typelist::typelist<ParametersBeforeTensorOptions...>, guts::typelist::typelist<ParametersAfterTensorOptions...>> final {
    static decltype(auto) wrapper(
                ParametersBeforeTensorOptions... parameters_before,
                optional<ScalarType> scalar_type,
                optional<Layout> layout,
                optional<Device> device,
                optional<bool> pin_memory,
                ParametersAfterTensorOptions... parameters_after) {
        return (*base_func_ptr)(
            std::forward<ParametersBeforeTensorOptions>(parameters_before)...,
            TensorOptions().dtype(scalar_type).device(device).layout(layout).pinned_memory(pin_memory),
            std::forward<ParametersAfterTensorOptions>(parameters_after)...
        );
    }
};

}

template<class FuncType, FuncType* _func_ptr>
struct hacky_wrapper_for_legacy_signatures final {
    static constexpr auto* func_ptr() {
        constexpr auto* result = detail::scatter_tensor_options<FuncType, _func_ptr>::func_ptr();
        return result;
    }
};

}
}