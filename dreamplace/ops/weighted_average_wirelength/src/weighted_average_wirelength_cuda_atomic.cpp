/**
 * @file   weighted_average_wirelength_cuda_atomic.cpp
 * @author Yibo Lin
 * @date   Aug 2018
 */
#include "utility/src/torch.h"
#include "utility/src/Msg.h"

DREAMPLACE_BEGIN_NAMESPACE

template <typename T, typename V>
int computeWeightedAverageWirelengthCudaAtomicLauncher(
        const T* x, const T* y, 
        const int* pin2net_map, 
        const unsigned char* net_mask, 
        int num_nets, 
        int num_pins, 
        const T* inv_gamma, 
        T* exp_xy, T* exp_nxy, 
        T* exp_xy_sum, T* exp_nxy_sum,
        T* xyexp_xy_sum, T* xyexp_nxy_sum,
        V* xy_max, V* xy_min, 
        T* partial_wl, // wirelength of each net 
        const T* grad_tensor, 
        T* grad_x_tensor, T* grad_y_tensor // the gradient is partial total wirelength to partial pin position  
        );

#define CHECK_FLAT(x) AT_ASSERTM(x.is_cuda() && x.ndimension() == 1, #x "must be a flat tensor on GPU")
#define CHECK_EVEN(x) AT_ASSERTM((x.numel()&1) == 0, #x "must have even number of elements")
#define CHECK_CONTIGUOUS(x) AT_ASSERTM(x.is_contiguous(), #x "must be contiguous")

typedef int V; 

/// @brief Compute weighted average wirelength and gradient. 
/// WL = \sum_i x_i*exp(x_i/gamma) / \sum_i exp(x_i/gamma) - \sum_i x_i*exp(-x_i/gamma) / \sum_i x_i*exp(-x_i/gamma), 
/// where x_i is pin location.
/// 
/// @param pos location of pins, x array followed by y array. 
/// @param pin2net_map map pin to net 
/// @param net_mask whether compute the wirelength for a net or not 
/// @param gamma gamma coefficient in weighted average wirelength. 
/// @return total wirelength cost.
std::vector<at::Tensor> weighted_average_wirelength_atomic_forward(
        at::Tensor pos,
        at::Tensor pin2net_map, 
        at::Tensor net_mask, 
        at::Tensor inv_gamma) 
{
    CHECK_FLAT(pos); 
    CHECK_EVEN(pos);
    CHECK_CONTIGUOUS(pos);
    CHECK_FLAT(pin2net_map);
    CHECK_CONTIGUOUS(pin2net_map);
    CHECK_FLAT(net_mask);
    CHECK_CONTIGUOUS(net_mask);

    int num_nets = net_mask.numel();
    int num_pins = pin2net_map.numel();

    // log-sum-exp for x, log-sum-exp for -x, log-sum-exp for y, log-sum-exp for -y 
    at::Tensor partial_wl = at::zeros({4, num_nets}, pos.options());
    at::Tensor exp_xy = at::zeros_like(pos);
    at::Tensor exp_nxy = at::zeros_like(pos);
    at::Tensor exp_xy_sum = at::zeros({2, num_nets}, pos.options());
    at::Tensor exp_nxy_sum = at::zeros({2, num_nets}, pos.options());
    at::Tensor xyexp_xy_sum = at::zeros({2, num_nets}, pos.options());
    at::Tensor xyexp_nxy_sum = at::zeros({2, num_nets}, pos.options());

    // it is ok for xy_max and xy_min to be integer 
    // we do not really need accurate max/min, just some values to scale x/y 
    // therefore, there is no need to scale xy_max and xy_min to improve accuracy 
    at::Tensor xy_max = at::full({2, num_nets}, std::numeric_limits<V>::min(), at::CUDA(at::kInt)); 
    at::Tensor xy_min = at::full({2, num_nets}, std::numeric_limits<V>::max(), at::CUDA(at::kInt)); 

    AT_DISPATCH_FLOATING_TYPES(pos.type(), "computeWeightedAverageWirelengthCudaAtomicLauncher", [&] {
            computeWeightedAverageWirelengthCudaAtomicLauncher<scalar_t, V>(
                    pos.data<scalar_t>(), pos.data<scalar_t>()+num_pins, 
                    pin2net_map.data<int>(), 
                    net_mask.data<unsigned char>(), 
                    num_nets, 
                    num_pins, 
                    inv_gamma.data<scalar_t>(), 
                    exp_xy.data<scalar_t>(), exp_nxy.data<scalar_t>(), 
                    exp_xy_sum.data<scalar_t>(), exp_nxy_sum.data<scalar_t>(),
                    xyexp_xy_sum.data<scalar_t>(), xyexp_nxy_sum.data<scalar_t>(), 
                    xy_max.data<V>(), xy_min.data<V>(), 
                    partial_wl.data<scalar_t>(), 
                    nullptr, 
                    nullptr, nullptr
                    );
            });

    // significant speedup is achieved by using summation in ATen 
    auto wl = at::sum(partial_wl); 
    return {wl, exp_xy, exp_nxy, exp_xy_sum, exp_nxy_sum, xyexp_xy_sum, xyexp_nxy_sum}; 
}

/// @brief Compute gradient 
/// @param grad_pos input gradient from backward propagation 
/// @param pos locations of pins 
/// @param exp_xy array of exp(x/gamma) and then exp(y/gamma)
/// @param exp_nxy array of exp(-x/gamma) and then exp(-y/gamma)
/// @param exp_xy_sum array of \sum(exp(x/gamma)) for each net and then \sum(exp(y/gamma))
/// @param exp_nxy_sum array of \sum(exp(-x/gamma)) for each net and then \sum(exp(-y/gamma))
/// @param xyexp_xy_sum array of \sum(x*exp(x/gamma)) for each net and then \sum(y*exp(y/gamma))
/// @param xyexp_nxy_sum array of \sum(x*exp(-x/gamma)) for each net and then \sum(y*exp(-y/gamma))
/// @param pin2net_map map pin to net 
/// @param net_mask an array to record whether compute the where for a net or not 
/// @param gamma a scalar tensor for the parameter in the equation 
at::Tensor weighted_average_wirelength_atomic_backward(
        at::Tensor grad_pos, 
        at::Tensor pos,
        at::Tensor exp_xy, at::Tensor exp_nxy, 
        at::Tensor exp_xy_sum, at::Tensor exp_nxy_sum, 
        at::Tensor xyexp_xy_sum, at::Tensor xyexp_nxy_sum, 
        at::Tensor pin2net_map, 
        at::Tensor net_mask, 
        at::Tensor inv_gamma) 
{
    CHECK_FLAT(pos); 
    CHECK_EVEN(pos);
    CHECK_CONTIGUOUS(pos);
    CHECK_FLAT(exp_xy); 
    CHECK_EVEN(exp_xy);
    CHECK_CONTIGUOUS(exp_xy);
    CHECK_FLAT(exp_nxy); 
    CHECK_EVEN(exp_nxy);
    CHECK_CONTIGUOUS(exp_nxy);
    CHECK_FLAT(exp_xy_sum); 
    CHECK_EVEN(exp_xy_sum);
    CHECK_CONTIGUOUS(exp_xy_sum);
    CHECK_FLAT(exp_nxy_sum); 
    CHECK_EVEN(exp_nxy_sum);
    CHECK_CONTIGUOUS(exp_nxy_sum);
    CHECK_FLAT(xyexp_xy_sum); 
    CHECK_EVEN(xyexp_xy_sum);
    CHECK_CONTIGUOUS(xyexp_xy_sum);
    CHECK_FLAT(xyexp_nxy_sum); 
    CHECK_EVEN(xyexp_nxy_sum);
    CHECK_CONTIGUOUS(xyexp_nxy_sum);
    CHECK_FLAT(pin2net_map);
    CHECK_CONTIGUOUS(pin2net_map);
    CHECK_FLAT(net_mask);
    CHECK_CONTIGUOUS(net_mask);
    at::Tensor grad_out = at::zeros_like(pos);

    int num_nets = net_mask.numel(); 
    int num_pins = pin2net_map.numel();

    AT_DISPATCH_FLOATING_TYPES(pos.type(), "computeWeightedAverageWirelengthCudaAtomicLauncher", [&] {
            computeWeightedAverageWirelengthCudaAtomicLauncher<scalar_t, V>(
                    pos.data<scalar_t>(), pos.data<scalar_t>()+num_pins, 
                    pin2net_map.data<int>(), 
                    net_mask.data<unsigned char>(), 
                    num_nets, 
                    num_pins, 
                    inv_gamma.data<scalar_t>(), 
                    exp_xy.data<scalar_t>(), exp_nxy.data<scalar_t>(), 
                    exp_xy_sum.data<scalar_t>(), exp_nxy_sum.data<scalar_t>(),
                    xyexp_xy_sum.data<scalar_t>(), xyexp_nxy_sum.data<scalar_t>(),
                    nullptr, nullptr, 
                    nullptr, 
                    grad_pos.data<scalar_t>(), 
                    grad_out.data<scalar_t>(), grad_out.data<scalar_t>()+num_pins
                    );
            });
    return grad_out; 
}

DREAMPLACE_END_NAMESPACE

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.def("forward", &DREAMPLACE_NAMESPACE::weighted_average_wirelength_atomic_forward, "WeightedAverageWirelength forward (CUDA)");
  m.def("backward", &DREAMPLACE_NAMESPACE::weighted_average_wirelength_atomic_backward, "WeightedAverageWirelength backward (CUDA)");
}
